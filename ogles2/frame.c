/*
 * Frame-level entries: aglSwapBuffers, glClear, glFlush, glFinish,
 * glViewport, glScissor, and the agl parameter calls (plan sections 6.4,
 * 7.2, 7.7).
 */
#include <utility/tagitem.h>
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
    if (!has_surface(ctx))
        return;
    ogles2_job_flush(ctx);
    ogles2_display_present(&ctx->display);
}

void ogles2_glFlush(struct OGLES2Context *ctx)
{
    if (has_surface(ctx))
        ogles2_job_flush(ctx);
}

void ogles2_glFinish(struct OGLES2Context *ctx)
{
    /* Phase 1 jobs finish before ogles2_job_flush returns (plan 7.1). */
    if (has_surface(ctx))
        ogles2_job_flush(ctx);
}

/* RGBA / UNSIGNED_BYTE, which is also the implementation's read format
 * (plan 7.2): the open job is rendered first, then the rows are read from
 * the render bitmap bottom-up (GL's origin is the bottom-left, 7.11).
 * Pixels outside the framebuffer are left as they are. */
void ogles2_glReadPixels(struct OGLES2Context *ctx, GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, void *pixels)
{
    LONG x0, y0, x1, y1;
    ULONG stride;
    if (!ogles2_context_valid(ctx))
        return;
    if ((format != GL_ALPHA && format != GL_RGB && format != GL_RGBA && format != GL_LUMINANCE &&
         format != GL_LUMINANCE_ALPHA) ||
        (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT_5_6_5 &&
         type != GL_UNSIGNED_SHORT_4_4_4_4 && type != GL_UNSIGNED_SHORT_5_5_5_1)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!has_surface(ctx) || !pixels || !ctx->display.bitmap)
        return;
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + width > (LONG)ctx->width ? (LONG)ctx->width : x + width;
    y1 = y + height > (LONG)ctx->height ? (LONG)ctx->height : y + height;
    if (x0 >= x1 || y0 >= y1)
        return;
    ogles2_job_flush(ctx);
    stride = ((ULONG)width * 4 + ctx->pack_alignment - 1) & ~(ULONG)(ctx->pack_alignment - 1);
    ogles2_display_read(&ctx->display, (UWORD)x0, (UWORD)y0, (UWORD)(x1 - x0), (UWORD)(y1 - y0),
                        (UBYTE *)pixels + (ULONG)(y0 - y) * stride + (ULONG)(x0 - x) * 4, stride);
}

void ogles2_glHint(struct OGLES2Context *ctx, GLenum target, GLenum mode)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_GENERATE_MIPMAP_HINT ||
        (mode != GL_FASTEST && mode != GL_NICEST && mode != GL_DONT_CARE)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->state.generate_mipmap_hint = mode; /* glGenerateMipmap has one way */
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
    if (!ctx->v3d || !mask)
        return;
    s = &ctx->state;
    /* The tile buffer clears whole buffers at the start of the job's render,
     * before any draw: a clear confined by the scissor box or by write
     * masks, or one after a draw of this job, is drawn as a rectangle
     * instead (plan 7.7). Depth and stencil are cleared together at the
     * start of every tile, but they hold nothing before a job's first draw
     * (job.c never loads them), so clearing only one there is still exact. */
    partial = ((s->caps & OGLES2_CAP_SCISSOR_TEST) &&
               (ctx->scissor[0] > 0 || ctx->scissor[1] > 0 ||
                ctx->scissor[0] + ctx->scissor[2] < (GLint)ctx->width ||
                ctx->scissor[1] + ctx->scissor[3] < (GLint)ctx->height)) ||
              ((mask & GL_COLOR_BUFFER_BIT) &&
               !(s->color_mask[0] && s->color_mask[1] && s->color_mask[2] && s->color_mask[3])) ||
              ((mask & GL_DEPTH_BUFFER_BIT) && !s->depth_mask) ||
              ((mask & GL_STENCIL_BUFFER_BIT) && (s->stencil_write_mask[0] & 0xff) != 0xff);
    if (partial || ogles2_job_has_draws(ctx))
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
}

void ogles2_aglSetParams2(struct OGLES2Context *ctx, struct TagItem *tags)
{
    struct TagItem *state = tags, *t;
    if (!ogles2_context_valid(ctx))
        return;
    while ((t = NextTagItem(&state))) {
        switch (t->ti_Tag) {
        case OGLES2_CCT_VSYNC:
            ctx->display.vsync = t->ti_Data ? TRUE : FALSE;
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
