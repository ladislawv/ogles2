/*
 * Contexts (plan section 6.4) and the entries every program calls first:
 * glGetError and glGetString. A context owns its surface (display.c) and its
 * GPU pools (v3dcore's V3DContext, device.c).
 */
#include <exec/memory.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <dos/var.h>
#include <proto/dos.h>
#include <libraries/ogles2.h>
#include "ogles2_internal.h"

void ogles2_error(struct OGLES2Context *ctx, GLenum error)
{
    if (ctx && ctx->error == GL_NO_ERROR)
        ctx->error = error;
}

/* Tags this library acts on; the rest are logged once per context. */
static BOOL known_tag(Tag t)
{
    switch (t) {
    case OGLES2_CCT_WINDOW:
    case OGLES2_CCT_BITMAP:
    case OGLES2_CCT_MODEID:
    case OGLES2_CCT_CONTEXT_FOR_MODEID:
    case OGLES2_CCT_DEPTH:
    case OGLES2_CCT_STENCIL:
    case OGLES2_CCT_VSYNC:
    case OGLES2_CCT_GET_WIDTH:
    case OGLES2_CCT_GET_HEIGHT:
    case OGLES2_CCT_PISTORM_STATE_ONLY:
    case OGLES2_CCT_PISTORM_WIDTH:
    case OGLES2_CCT_PISTORM_HEIGHT:
    case OGLES2_CCT_PISTORM_GET_WINDOW:
    case OGLES2_CCT_PISTORM_MSAA:
    case OGLES2_CCT_DEBUG_SHADER_LOG:
    case OGLES2_CCT_SHARE_WITH:
        return TRUE;
    default:
        return FALSE;
    }
}

struct OGLES2Context *ogles2_aglCreateContext2(ULONG *errcode, struct TagItem *tags)
{
    struct OGLES2Context *ctx;
    struct TagItem *state = tags, *t;
    BOOL state_only = GetTagData(OGLES2_CCT_PISTORM_STATE_ONLY, FALSE, tags) ? TRUE : FALSE;
    ULONG depth = GetTagData(OGLES2_CCT_DEPTH, 0, tags);
    ULONG stencil = GetTagData(OGLES2_CCT_STENCIL, 0, tags);
    BOOL debug_shader_log = GetTagData(OGLES2_CCT_DEBUG_SHADER_LOG, FALSE, tags) ? TRUE : FALSE;
    struct OGLES2Context *with = (struct OGLES2Context *)GetTagData(OGLES2_CCT_SHARE_WITH, 0, tags);
    ULONG why = OGLES2_CCE_OK;

    if (with && !ogles2_context_valid(with)) {
        ogles2_log("aglCreateContext2: OGLES2_CCT_SHARE_WITH 0x%08lx is not a context", (ULONG)with);
        why = OGLES2_CCE_BAD_TAGS;
        goto fail;
    }
    /* Opened even for a state-only context, so GL_RENDERER is real when a
     * GPU exists. */
    if (!ogles2_device_open() && !state_only) {
        why = OGLES2_CCE_NO_GPU;
        goto fail;
    }
    ctx = AllocVec(sizeof(*ctx), MEMF_ANY | MEMF_CLEAR);
    if (!ctx) {
        why = OGLES2_CCE_NOMEM;
        goto fail;
    }
    ctx->magic = OGLES2_CONTEXT_MAGIC;
    ctx->debug_shader_log = debug_shader_log;
    ctx->error = GL_NO_ERROR;
    ogles2_state_init(ctx);
    /* The objects: with's group, or one of its own (share.c). */
    if (!ogles2_share_join(ctx, with)) {
        why = OGLES2_CCE_NOMEM;
        goto fail_ctx;
    }

    while ((t = NextTagItem(&state)))
        if (!known_tag(t->ti_Tag))
            ogles2_log("aglCreateContext2: tag 0x%08lx = 0x%08lx not supported yet", t->ti_Tag,
                       t->ti_Data);

    if (!state_only) {
        int v3d_depth;
        char var[16];
        if (GetVar((STRPTR) "OGLES2/Probe", (STRPTR)var, sizeof(var), GVF_GLOBAL_ONLY) > 0) {
            const char *p;
            for (p = var; *p; p++) {
                if (*p == 'D')
                    ctx->probe |= OGLES2_PROBE_STORE_DITHER;
                else if (*p == 'C')
                    ctx->probe |= OGLES2_PROBE_LOG_CLEAR;
                else if (*p == 'Y')
                    ctx->probe |= OGLES2_PROBE_FLIP_Y;
                else if (*p == 'S')
                    ctx->probe |= OGLES2_PROBE_SEMAPHORE;
                else if (*p == 'T')
                    ctx->probe |= OGLES2_PROBE_TRACE;
                else if (*p == 'P')
                    ctx->probe |= OGLES2_PROBE_PERIODIC;
                else if (*p == 'A')
                    ctx->probe |= OGLES2_PROBE_ADDR;
                else if (*p == 'M')
                    ctx->probe |= OGLES2_PROBE_CPU_MIPMAP;
                else if (*p == 'U')
                    ctx->probe |= OGLES2_PROBE_CPU_UPLOAD;
                else if (*p == 'K')
                    ctx->probe |= OGLES2_PROBE_CPU_COPY;
            }
            ogles2_log("aglCreateContext2: probes \"%s\" (ENV:OGLES2/Probe)", var);
            if ((ctx->probe & OGLES2_PROBE_ADDR) &&
                GetVar((STRPTR) "OGLES2/ProbeAddr", (STRPTR)var, sizeof(var), GVF_GLOBAL_ONLY) > 0) {
                const char *p;
                for (p = var; *p; p++) {
                    int dgt = *p >= '0' && *p <= '9' ? *p - '0'
                            : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10
                            : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
                    if (dgt >= 0)
                        ctx->probe_addr = ctx->probe_addr * 16 + (ULONG)dgt;
                }
                ogles2_log("aglCreateContext2: probe A: positions at 0x%08lx (%s)", ctx->probe_addr,
                           (ULONG)(ogles2_device_gpu_reachable(ctx->probe_addr, 4096)
                                       ? "inside the GPU-visible RAM"
                                       : "OUTSIDE the GPU-visible RAM"));
            }
        }
        if (!ogles2_display_open(&ctx->display, tags, &why))
            goto fail_ctx;
        ctx->width = ctx->display.width;
        ctx->height = ctx->display.height;
        /* Plan section 7.2: any stencil gives D24S8; otherwise up to 16 bits
         * of depth D16, more D32F. The tile buffer holds them; the default
         * framebuffer never stores them (job.c). */
        if (stencil) {
            ctx->depth_bits = 24;
            ctx->stencil_bits = 8;
            ctx->depth_format = OGLES2_DEPTH_24;
            v3d_depth = 32;
        } else if (depth == 0) {
            ctx->depth_bits = 0;
            ctx->depth_format = OGLES2_DEPTH_16;
            v3d_depth = 16; /* the smallest pools; depth is never used */
        } else if (depth <= 16) {
            ctx->depth_bits = 16;
            ctx->depth_format = OGLES2_DEPTH_16;
            v3d_depth = 16;
        } else {
            ctx->depth_bits = 32;
            ctx->depth_format = OGLES2_DEPTH_32F;
            v3d_depth = 32;
        }
        /* 4x MSAA (plan 7.2): the tag, else ENV:OGLES2/MSAA. */
        if ((t = FindTagItem(OGLES2_CCT_PISTORM_MSAA, tags)) != 0)
            ctx->msaa = t->ti_Data >= 2;
        else if (GetVar((STRPTR) "OGLES2/MSAA", (STRPTR)var, sizeof(var), GVF_GLOBAL_ONLY) > 0)
            ctx->msaa = var[0] >= '2' && var[0] <= '9';
        ctx->v3d = ogles2_device_context_new((UWORD)ctx->width, (UWORD)ctx->height, v3d_depth, ctx->msaa);
        if (!ctx->v3d) {
            ogles2_display_close(&ctx->display);
            why = OGLES2_CCE_NOMEM;
            goto fail_ctx;
        }
        ObtainSemaphore(&OGLES2LibBase->lock);
        OGLES2LibBase->contexts++;
        ReleaseSemaphore(&OGLES2LibBase->lock);
    }
    ctx->default_draw_buffer = GL_BACK; /* EXT_draw_buffers */
    ogles2_fb_update(ctx); /* the default framebuffer is drawn into */
    ctx->viewport[2] = ctx->scissor[2] = (GLint)ctx->width;
    ctx->viewport[3] = ctx->scissor[3] = (GLint)ctx->height;

    if ((t = FindTagItem(OGLES2_CCT_GET_WIDTH, tags)) && t->ti_Data)
        *(ULONG *)t->ti_Data = ctx->width;
    if ((t = FindTagItem(OGLES2_CCT_GET_HEIGHT, tags)) && t->ti_Data)
        *(ULONG *)t->ti_Data = ctx->height;
    if ((t = FindTagItem(OGLES2_CCT_PISTORM_GET_WINDOW, tags)) && t->ti_Data)
        *(struct Window **)t->ti_Data =
            ctx->display.input_window ? ctx->display.input_window : ctx->display.window;
    ogles2_log("aglCreateContext2: context 0x%08lx, %ldx%ld%s", (ULONG)ctx, ctx->width, ctx->height,
               (ULONG)(state_only ? ", state only" : ""));
    if (with)
        ogles2_log("aglCreateContext2: context 0x%08lx shares objects with 0x%08lx (%ld in the group)",
                   (ULONG)ctx, (ULONG)with, ctx->share->members);
    if (errcode)
        *errcode = OGLES2_CCE_OK;
    return ctx;

fail_ctx:
    ogles2_share_leave(ctx); /* nothing made yet: a group of its own is freed */
    ctx->magic = 0;
    FreeVec(ctx);
fail:
    ogles2_log("aglCreateContext2: failed, error %ld", why);
    if (errcode)
        *errcode = why;
    return 0;
}

void ogles2_aglDestroyContext(struct OGLES2Context *ctx)
{
    struct SignalSemaphore *group;
    if (!ogles2_context_valid(ctx))
        return;
    /* A member of a shared group goes under the group's semaphore, as every
     * call into a member (share.c). */
    if ((group = ctx->share_lock))
        ObtainSemaphore(group);
    ogles2_log("aglDestroyContext: context 0x%08lx: freeing", (ULONG)ctx);
    ogles2_job_discard(ctx);
    ogles2_trace(ctx, "aglDestroyContext: the job finished", -1, 0);
    ogles2_draw_free(ctx);
    ogles2_job_free(ctx);
    ogles2_trace(ctx, "aglDestroyContext: draw and job state freed", -1, 0);
    ogles2_objects_free(ctx);
    ogles2_trace(ctx, "aglDestroyContext: programs and shaders freed", -1, 0);
    ogles2_fbo_free(ctx); /* before the textures: renderbuffers' colour storage is theirs */
    ogles2_textures_free(ctx);
    ogles2_trace(ctx, "aglDestroyContext: framebuffers and textures freed", -1, 0);
    ogles2_buffers_free(ctx);
    ogles2_trace(ctx, "aglDestroyContext: buffers freed", -1, 0);
    /* The modules freed the shared objects if it was the group's last
     * member (share.c), else only what is the context's own. A group with
     * other members stays, its semaphore with it. */
    ogles2_share_leave(ctx);
    if (group)
        ReleaseSemaphore(group);
    if (ctx->v3d) {
        ObtainSemaphore(&OGLES2LibBase->lock);
        OGLES2LibBase->contexts--;
        if (OGLES2LibBase->pend_ctx == ctx) /* ogles2_job_discard waited for it */
            OGLES2LibBase->pend_ctx = 0;
        ReleaseSemaphore(&OGLES2LibBase->lock);
    }
    ogles2_device_context_free(ctx->v3d);
    ogles2_display_close(&ctx->display);
    ogles2_log("aglDestroyContext: context 0x%08lx (%lu frames, %lu dropped, %lu binning blocks added)",
               (ULONG)ctx, ctx->frames, ctx->frames_dropped, ctx->chained_blocks);
    if (ctx->n_jobs)
        ogles2_log("timing: %lu jobs, each on average: %lu us of CPU work before its flush, %lu us "
                   "waiting for binning, %lu us for the render",
                   ctx->n_jobs, (ULONG)(ctx->sum_build / ctx->n_jobs), (ULONG)(ctx->sum_bin / ctx->n_jobs),
                   (ULONG)(ctx->sum_render / ctx->n_jobs));
    if (ctx->n_jobs)
        ogles2_log("timing: and in each flush %lu us building the render list, %lu us for the CPU cache "
                   "before the GPU, %lu us after",
                   (ULONG)(ctx->sum_list / ctx->n_jobs), (ULONG)(ctx->sum_pre / ctx->n_jobs),
                   (ULONG)(ctx->sum_post / ctx->n_jobs));
    if (ctx->n_swaps > 1)
        ogles2_log("timing: %lu swaps, %lu us from one to the next on average, %lu us of it presenting",
                   ctx->n_swaps, (ULONG)(ctx->sum_frame / (ctx->n_swaps - 1)),
                   (ULONG)(ctx->sum_present / ctx->n_swaps));
    if (ctx->n_swaps) {
        /* Uploads as totals: most happen while the program loads. */
        ogles2_log("timing: per frame %lu draws, %lu us in them; in all %lu ms in glBuffer(Sub)Data, "
                   "%lu ms in glTex(Sub)Image2D",
                   ctx->n_draws / ctx->n_swaps, (ULONG)(ctx->sum_draw / ctx->n_swaps),
                   (ULONG)(ctx->sum_bufdata / 1000), (ULONG)(ctx->sum_teximage / 1000));
        if (ctx->probe & OGLES2_PROBE_PERIODIC)
            ogles2_log("timing: per frame in draws: %lu us checks, %lu shaders, %lu textures, %lu uniforms, "
                       "%lu vertex data, %lu record, %lu indices, %lu binning list",
                       (ULONG)(ctx->stage[0] / ctx->n_swaps), (ULONG)(ctx->stage[1] / ctx->n_swaps),
                       (ULONG)(ctx->stage[2] / ctx->n_swaps), (ULONG)(ctx->stage[3] / ctx->n_swaps),
                       (ULONG)(ctx->stage[4] / ctx->n_swaps), (ULONG)(ctx->stage[5] / ctx->n_swaps),
                       (ULONG)(ctx->stage[6] / ctx->n_swaps), (ULONG)(ctx->stage[7] / ctx->n_swaps));
        if ((ctx->probe & OGLES2_PROBE_PERIODIC) && ctx->n_draws)
            ogles2_log("timing: per draw %lu uniform stream words, %lu bytes of vertex data, %lu indices",
                       (ULONG)(ctx->n_uwords / ctx->n_draws), (ULONG)(ctx->n_vbytes / ctx->n_draws),
                       (ULONG)(ctx->n_indices / ctx->n_draws));
    }
    ctx->magic = 0;
    FreeVec(ctx);
    ogles2_log_flush(); /* the caller is the application: DOS calls are fine */
}

void ogles2_aglMakeCurrent(struct OGLES2Context *ctx)
{
    /* The current context lives in the client (plan section 6.2). */
    (void)ctx;
}

GLenum ogles2_glGetError(struct OGLES2Context *ctx)
{
    GLenum e;
    if (!ogles2_context_valid(ctx))
        return GL_NO_ERROR;
    e = ctx->error;
    ctx->error = GL_NO_ERROR;
    return e;
}

const GLubyte *ogles2_glGetString(struct OGLES2Context *ctx, GLenum name)
{
    if (!ogles2_context_valid(ctx))
        return 0;
    switch (name) {
    case GL_VENDOR:
        return (const GLubyte *)"ogles2.library";
    case GL_RENDERER:
        return (const GLubyte *)ogles2_device_renderer();
    case GL_VERSION: /* must begin "OpenGL ES 2.0" (plan section 7.10) */
        return (const GLubyte *)"OpenGL ES 2.0 ogles2.library 1.3";
    case GL_SHADING_LANGUAGE_VERSION: /* must begin "OpenGL ES GLSL ES 1.00" */
        return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
    case GL_EXTENSIONS:
        return (const GLubyte *)"GL_OES_element_index_uint GL_OES_compressed_ETC1_RGB8_texture GL_EXT_texture_format_BGRA8888 "
                                 "GL_OES_mapbuffer GL_EXT_blend_minmax GL_OES_draw_elements_base_vertex "
                                 "GL_OES_rgb8_rgba8 GL_OES_packed_depth_stencil GL_OES_fbo_render_mipmap "
                                 "GL_EXT_discard_framebuffer GL_OES_texture_npot GL_EXT_read_format_bgra "
                                 "GL_EXT_texture_filter_anisotropic GL_EXT_texture_lod_bias GL_SGIS_texture_lod "
                                 "GL_ARB_texture_mirror_clamp_to_edge GL_OES_standard_derivatives "
                                 "GL_OES_get_program_binary GL_OES_texture_float GL_OES_texture_half_float "
                                 "GL_OES_texture_half_float_linear GL_OES_depth_texture GL_ARB_texture_rectangle "
                                 "GL_EXT_debug_marker GL_EXT_texture_compression_dxt1 GL_EXT_texture_compression_s3tc "
                                 "GL_ANGLE_texture_compression_dxt3 GL_ANGLE_texture_compression_dxt5 GL_EXT_clip_control "
                                 "GL_EXT_draw_buffers GL_NV_fbo_color_attachments GL_OES_required_internalformat "
                                 "GL_EXT_color_buffer_half_float";
    default:
        ogles2_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
}

/* Generated stubs (stubs.c) call this for entries not written yet. Each name
 * is logged once; the stubs pass string literals, so the pointer identifies
 * the entry. */
void ogles2_unimplemented(struct OGLES2Context *ctx, const char *name)
{
    static const char *seen[64];
    static ULONG nseen;
    ULONG i;
    BOOL first = TRUE;

    ogles2_error(ogles2_context_valid(ctx) ? ctx : 0, GL_INVALID_OPERATION);
    Forbid();
    for (i = 0; i < nseen; i++)
        if (seen[i] == name)
            first = FALSE;
    if (first && nseen < 64)
        seen[nseen++] = name;
    Permit();
    if (first)
        ogles2_log("not implemented yet: %s", (ULONG)name);
}
