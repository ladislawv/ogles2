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

BOOL ogles2_context_valid(struct OGLES2Context *ctx)
{
    return ctx && ctx->magic == OGLES2_CONTEXT_MAGIC;
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
    ULONG why = OGLES2_CCE_OK;

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
    ctx->error = GL_NO_ERROR;
    ogles2_state_init(ctx);

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
                else if (*p == 'A')
                    ctx->probe |= OGLES2_PROBE_ADDR;
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
        ctx->v3d = ogles2_device_context_new((UWORD)ctx->width, (UWORD)ctx->height, v3d_depth);
        if (!ctx->v3d) {
            ogles2_display_close(&ctx->display);
            why = OGLES2_CCE_NOMEM;
            goto fail_ctx;
        }
    }
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
    if (errcode)
        *errcode = OGLES2_CCE_OK;
    return ctx;

fail_ctx:
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
    if (!ogles2_context_valid(ctx))
        return;
    ogles2_job_discard(ctx);
    ogles2_draw_free(ctx);
    ogles2_objects_free(ctx);
    ogles2_textures_free(ctx);
    ogles2_buffers_free(ctx);
    ogles2_device_context_free(ctx->v3d);
    ogles2_display_close(&ctx->display);
    ogles2_log("aglDestroyContext: context 0x%08lx (%lu frames, %lu dropped)", (ULONG)ctx,
               ctx->frames, ctx->frames_dropped);
    ctx->magic = 0;
    FreeVec(ctx);
    ogles2_log_flush(); /* the caller is the application: DOS calls are fine */
}

void ogles2_aglMakeCurrent(struct OGLES2Context *ctx)
{
    /* The current context lives in the client (plan section 6.2); the
     * library learns of it here to bind the display state (M1). */
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
        return (const GLubyte *)"OpenGL ES 2.0 ogles2.library 1.0";
    case GL_SHADING_LANGUAGE_VERSION: /* must begin "OpenGL ES GLSL ES 1.00" */
        return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
    case GL_EXTENSIONS:
        return (const GLubyte *)"GL_OES_element_index_uint";
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
