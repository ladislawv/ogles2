/*
 * Frame-level entries: aglSwapBuffers, glClear, glFlush, glFinish,
 * glViewport, glScissor, and the agl parameter calls (plan sections 6.4,
 * 7.2, 7.7).
 */
#include <exec/memory.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <libraries/ogles2.h>
#include "ogles2_internal.h"

/* A state-only (test) context has no surface: frame entries do nothing. */
static BOOL has_surface(struct OGLES2Context *ctx)
{
    return ogles2_context_valid(ctx) && ctx->v3d;
}

void ogles2_aglSwapBuffers(struct OGLES2Context *ctx)
{
    ULONG t0, t1;
    if (!has_surface(ctx))
        return;
    /* Vsync on a screen of our own: the frame goes out at the refresh
     * (display.c). */
    if (ctx->display.vsync && ctx->display.own_screen)
        ogles2_display_wait_refresh(&ctx->display);
    ogles2_job_flush(ctx, TRUE);
    ogles2_display_wait_done(&ctx->display);
    t0 = ogles2_clock_us();
    ogles2_display_present(&ctx->display);
    t1 = ogles2_clock_us();
    ctx->sum_present += (double)(ULONG)(t1 - t0);
    if (ctx->n_swaps++)
        ctx->sum_frame += (double)(ULONG)(t1 - ctx->t_last_swap);
    ctx->t_last_swap = t1;
    /* Probe P: the last 256 frames, to see a program slow down over time. */
    if ((ctx->probe & OGLES2_PROBE_PERIODIC) && !(ctx->n_swaps & 255)) {
        ULONG jobs = ctx->n_jobs - ctx->p_jobs;
        if (!jobs)
            jobs = 1;
        ogles2_log("periodic: frames to %lu: %lu us a frame, %lu jobs; per job %lu us of CPU work, %lu us "
                   "render list, %lu us cache, %lu us render wait; %lu textures renamed; %lu bytes of GPU memory",
                   ctx->n_swaps, (ULONG)((ctx->sum_frame - ctx->p_frame) / 256), jobs,
                   (ULONG)((ctx->sum_build - ctx->p_build) / jobs), (ULONG)((ctx->sum_list - ctx->p_list) / jobs),
                   (ULONG)((ctx->sum_pre - ctx->p_pre) / jobs), (ULONG)((ctx->sum_render - ctx->p_render) / jobs),
                   ctx->n_renames - ctx->p_renames, ogles2_device_bytes());
        ogles2_log("periodic: per frame %lu draws, %lu us in them, %lu us in glBuffer(Sub)Data, %lu us in "
                   "glTex(Sub)Image2D",
                   (ctx->n_draws - ctx->p_draws) / 256, (ULONG)((ctx->sum_draw - ctx->p_draw) / 256),
                   (ULONG)((ctx->sum_bufdata - ctx->p_bufdata) / 256),
                   (ULONG)((ctx->sum_teximage - ctx->p_teximage) / 256));
        ogles2_log("periodic: per frame in draws: %lu us checks, %lu shaders, %lu textures, %lu uniforms, "
                   "%lu vertex data, %lu record, %lu indices, %lu binning list",
                   (ULONG)((ctx->stage[0] - ctx->p_stage[0]) / 256), (ULONG)((ctx->stage[1] - ctx->p_stage[1]) / 256),
                   (ULONG)((ctx->stage[2] - ctx->p_stage[2]) / 256), (ULONG)((ctx->stage[3] - ctx->p_stage[3]) / 256),
                   (ULONG)((ctx->stage[4] - ctx->p_stage[4]) / 256), (ULONG)((ctx->stage[5] - ctx->p_stage[5]) / 256),
                   (ULONG)((ctx->stage[6] - ctx->p_stage[6]) / 256), (ULONG)((ctx->stage[7] - ctx->p_stage[7]) / 256));
        {
            int k;
            for (k = 0; k < 8; k++)
                ctx->p_stage[k] = ctx->stage[k];
        }
        ctx->p_draws = ctx->n_draws;
        ctx->p_draw = ctx->sum_draw;
        ctx->p_bufdata = ctx->sum_bufdata;
        ctx->p_teximage = ctx->sum_teximage;
        ctx->p_frame = ctx->sum_frame;
        ctx->p_build = ctx->sum_build;
        ctx->p_list = ctx->sum_list;
        ctx->p_pre = ctx->sum_pre;
        ctx->p_render = ctx->sum_render;
        ctx->p_jobs = ctx->n_jobs;
        ctx->p_renames = ctx->n_renames;
    }
}

void ogles2_glFlush(struct OGLES2Context *ctx)
{
    if (has_surface(ctx))
        ogles2_job_flush(ctx, FALSE);
}

void ogles2_glFinish(struct OGLES2Context *ctx)
{
    if (has_surface(ctx)) {
        ogles2_job_flush(ctx, FALSE);
        ogles2_job_finish(ctx); /* a flush may leave its render running (M6) */
    }
}

/* RGBA / UNSIGNED_BYTE, which is also the implementation's read format
 * (plan 7.2), and EXT_read_format_bgra's BGRA with unsigned bytes or the
 * two reversed 16-bit packings: the open job is rendered first, then the
 * rows are read from the render bitmap bottom-up (GL's origin is the
 * bottom-left, 7.11), or from a framebuffer object's colour image (fbo.c;
 * its row 0 is GL's). Pixels outside the framebuffer are left as they are. */
void ogles2_glReadPixels(struct OGLES2Context *ctx, GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, void *pixels)
{
    LONG x0, y0, x1, y1;
    ULONG stride, bpp, rgba_stride;
    UBYTE *rgba, *first;
    BOOL ok = TRUE;
    if (!ogles2_context_valid(ctx))
        return;
    if ((format != GL_ALPHA && format != GL_RGB && format != GL_RGBA && format != GL_LUMINANCE &&
         format != GL_LUMINANCE_ALPHA && format != GL_BGRA_EXT) ||
        (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT_5_6_5 &&
         type != GL_UNSIGNED_SHORT_4_4_4_4 && type != GL_UNSIGNED_SHORT_5_5_5_1 &&
         type != GL_UNSIGNED_SHORT_4_4_4_4_REV_EXT && type != GL_UNSIGNED_SHORT_1_5_5_5_REV_EXT)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(format == GL_RGBA && type == GL_UNSIGNED_BYTE) &&
        !(format == GL_BGRA_EXT && (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT_4_4_4_4_REV_EXT ||
                                    type == GL_UNSIGNED_SHORT_1_5_5_5_REV_EXT))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!has_surface(ctx) || !ogles2_fb_ready(ctx) || !pixels || !ctx->display.bitmap)
        return;
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + width > (LONG)ctx->fb_w ? (LONG)ctx->fb_w : x + width;
    y1 = y + height > (LONG)ctx->fb_h ? (LONG)ctx->fb_h : y + height;
    if (x0 >= x1 || y0 >= y1)
        return;
    ogles2_job_flush(ctx, FALSE);
    ogles2_job_finish(ctx);
    bpp = type == GL_UNSIGNED_BYTE ? 4 : 2;
    stride = ((ULONG)width * bpp + ctx->pack_alignment - 1) & ~(ULONG)(ctx->pack_alignment - 1);
    first = (UBYTE *)pixels + (ULONG)(y0 - y) * stride + (ULONG)(x0 - x) * bpp;
    /* RGBA bytes: straight into the application's memory; the 16-bit
     * packings through a copy of the region. */
    if (bpp == 4) {
        rgba = first;
        rgba_stride = stride;
    } else {
        rgba_stride = (ULONG)(x1 - x0) * 4;
        if (!(rgba = AllocVec(rgba_stride * (ULONG)(y1 - y0), MEMF_ANY))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
    }
    if (ctx->draw_fb)
        ok = ogles2_fbo_read(ctx, (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0), rgba, rgba_stride);
    else
        ogles2_display_read(&ctx->display, (UWORD)x0, (UWORD)y0, (UWORD)(x1 - x0), (UWORD)(y1 - y0),
                            rgba, rgba_stride);
    if (!ok)
        ogles2_error(ctx, GL_INVALID_OPERATION); /* an object without a colour image */
    else if (format == GL_BGRA_EXT) {
        LONG i, j;
        for (j = 0; j < y1 - y0; j++) {
            UBYTE *s = rgba + (ULONG)j * rgba_stride, *d = first + (ULONG)j * stride;
            for (i = 0; i < x1 - x0; i++, s += 4) {
                UBYTE r = s[0], g = s[1], b = s[2], a = s[3];
                UWORD v;
                if (bpp == 4) { /* in place: B, G, R, A */
                    s[0] = b;
                    s[2] = r;
                    continue;
                }
                /* The reversed packings hold blue in the lowest bits (EXT_read_format_bgra). */
                if (type == GL_UNSIGNED_SHORT_4_4_4_4_REV_EXT)
                    v = (UWORD)((b >> 4) | ((g >> 4) << 4) | ((r >> 4) << 8) | ((a >> 4) << 12));
                else
                    v = (UWORD)((b >> 3) | ((g >> 3) << 5) | ((r >> 3) << 10) | ((a >> 7) << 15));
                *(UWORD *)d = v; /* the 68k's own byte order, as GL's packed types are */
                d += 2;
            }
        }
    }
    if (bpp != 4)
        FreeVec(rgba);
}

void ogles2_glHint(struct OGLES2Context *ctx, GLenum target, GLenum mode)
{
    if (!ogles2_context_valid(ctx))
        return;
    if ((target != GL_GENERATE_MIPMAP_HINT && target != GL_FRAGMENT_SHADER_DERIVATIVE_HINT_OES) ||
        (mode != GL_FASTEST && mode != GL_NICEST && mode != GL_DONT_CARE)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* glGenerateMipmap has one way, and the compiled shaders one way of
     * computing derivatives (OES_standard_derivatives): kept for the queries. */
    if (target == GL_GENERATE_MIPMAP_HINT)
        ctx->state.generate_mipmap_hint = mode;
    else
        ctx->state.derivative_hint = mode;
}

void ogles2_glClear(struct OGLES2Context *ctx, GLbitfield mask)
{
    const struct OGLES2State *s;
    BOOL partial;

    if (!ogles2_context_valid(ctx))
        return;
    if (mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ctx->v3d || !mask || !ogles2_fb_ready(ctx))
        return;
    s = &ctx->state;
    /* The tile buffer clears whole buffers at the start of the job's render,
     * before any draw: a clear confined by the scissor box or by write
     * masks, or one after a draw of this job, is drawn as a rectangle
     * instead (plan 7.7). Depth and stencil are cleared together at the
     * start of every tile; unless they were loaded (kept from a mid-frame
     * flush, or a framebuffer object's) they hold nothing before a job's
     * first draw, so clearing only one there is still exact. */
    partial = ((s->caps & OGLES2_CAP_SCISSOR_TEST) &&
               (ctx->scissor[0] > 0 || ctx->scissor[1] > 0 ||
                ctx->scissor[0] + ctx->scissor[2] < (GLint)ctx->fb_w ||
                ctx->scissor[1] + ctx->scissor[3] < (GLint)ctx->fb_h)) ||
              ((mask & GL_COLOR_BUFFER_BIT) &&
               !(s->color_mask[0] && s->color_mask[1] && s->color_mask[2] && s->color_mask[3])) ||
              ((mask & GL_DEPTH_BUFFER_BIT) && !s->depth_mask) ||
              ((mask & GL_STENCIL_BUFFER_BIT) && (s->stencil_write_mask[0] & 0xff) != 0xff) ||
              /* depth and stencil loaded (job.c): the tile buffer clears
               * both or neither */
              ((ctx->draw_fb || ctx->zs_saved) && ctx->fb_stencil_bits && ctx->fb_depth_bits &&
               (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) &&
               (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) !=
                   (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
    /* A job with draws for this framebuffer: too late for its tile clear. */
    if (partial || (ogles2_job_has_draws(ctx) && ctx->job_fb == ctx->draw_fb))
        ogles2_draw_clear(ctx, mask);
    else
        ogles2_job_clear(ctx, mask);
}

void ogles2_glViewport(struct OGLES2Context *ctx, GLint x, GLint y, GLsizei width, GLsizei height)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* Clamped to the implementation maximum (GL_MAX_VIEWPORT_DIMS). */
    ctx->viewport[0] = x;
    ctx->viewport[1] = y;
    ctx->viewport[2] = width > OGLES2_MAX_VIEWPORT ? OGLES2_MAX_VIEWPORT : width;
    ctx->viewport[3] = height > OGLES2_MAX_VIEWPORT ? OGLES2_MAX_VIEWPORT : height;
    ctx->state_ver++;
}

void ogles2_glScissor(struct OGLES2Context *ctx, GLint x, GLint y, GLsizei width, GLsizei height)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    ctx->scissor[0] = x;
    ctx->scissor[1] = y;
    ctx->scissor[2] = width;
    ctx->scissor[3] = height;
    ctx->state_ver++;
}

/* Retarget a bitmap context (OGLES2_CCT_BITMAP): the open frame is rendered
 * into the old bitmap first; from then on the context renders into the new
 * one, its default framebuffer resized to it. As after an EGL surface
 * resize, the viewport and scissor stay as the application set them. */
void ogles2_aglSetBitmap(struct OGLES2Context *ctx, struct BitMap *bitmap)
{
    struct OGLES2Display old;
    if (!has_surface(ctx))
        return;
    if (bitmap == ctx->display.bitmap)
        return;
    ogles2_job_flush(ctx, TRUE);
    ogles2_job_finish(ctx);
    old = ctx->display;
    if (!ogles2_display_set_bitmap(&ctx->display, bitmap))
        return;
    if (ctx->display.width != old.width || ctx->display.height != old.height) {
        ogles2_job_free(ctx); /* the depth/stencil save is the old size */
        if (!ogles2_device_context_resize(ctx->v3d, ctx->display.width, ctx->display.height)) {
            ctx->display = old; /* keep rendering into the old bitmap */
            return;
        }
        ctx->width = ctx->display.width;
        ctx->height = ctx->display.height;
    }
    ogles2_fb_update(ctx);
    ogles2_log("aglSetBitmap: bitmap 0x%08lx, %ldx%ld", (ULONG)bitmap, ctx->width, ctx->height);
}

/* aglSetParams2(OGLES2_CCT_WINDOW): another window, or the same one
 * resized; the default framebuffer follows its size, as aglSetBitmap. */
static void set_window(struct OGLES2Context *ctx, struct Window *w)
{
    struct OGLES2Display old;
    if (!has_surface(ctx))
        return;
    ogles2_job_flush(ctx, TRUE);
    ogles2_job_finish(ctx);
    old = ctx->display;
    if (!ogles2_display_set_window(&ctx->display, w))
        return;
    if (ctx->display.bitmap == old.bitmap)
        return; /* the same window at the same size */
    if (ctx->display.width != old.width || ctx->display.height != old.height) {
        ogles2_job_free(ctx); /* the depth/stencil save is the old size */
        if (!ogles2_device_context_resize(ctx->v3d, ctx->display.width, ctx->display.height)) {
            ogles2_display_drop(&ctx->display); /* the new bitmap; the old display stays */
            ctx->display = old;
            return;
        }
        ctx->width = ctx->display.width;
        ctx->height = ctx->display.height;
    }
    ogles2_display_drop(&old);
    ogles2_fb_update(ctx);
    ogles2_log("aglSetParams2: window 0x%08lx, %ldx%ld", (ULONG)w, ctx->width, ctx->height);
}

void ogles2_aglSetParams2(struct OGLES2Context *ctx, struct TagItem *tags)
{
    struct TagItem *state = tags, *t;
    if (!ogles2_context_valid(ctx))
        return;
    while ((t = NextTagItem(&state))) {
        switch (t->ti_Tag) {
        case OGLES2_CCT_VSYNC:
            ogles2_display_set_vsync(&ctx->display, t->ti_Data ? TRUE : FALSE);
            break;
        case OGLES2_CCT_WINDOW:
            set_window(ctx, (struct Window *)t->ti_Data);
            break;
        case OGLES2_CCT_GET_WIDTH:
            if (t->ti_Data)
                *(ULONG *)t->ti_Data = ctx->width;
            break;
        case OGLES2_CCT_GET_HEIGHT:
            if (t->ti_Data)
                *(ULONG *)t->ti_Data = ctx->height;
            break;
        case OGLES2_CCT_PISTORM_GET_WINDOW:
            if (t->ti_Data)
                *(struct Window **)t->ti_Data =
                    ctx->display.input_window ? ctx->display.input_window : ctx->display.window;
            break;
        default:
            /* Retargeting and resizing come later in M1. */
            ogles2_log("aglSetParams2: tag 0x%08lx = 0x%08lx not supported yet", t->ti_Tag,
                       t->ti_Data);
            break;
        }
    }
}
