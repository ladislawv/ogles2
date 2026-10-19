/*
 * glCopyTexImage2D and glCopyTexSubImage2D on the GPU (GitHub issue 5:
 * OpenLara copies its 640x480 screen into a refraction texture every
 * frame; through the CPU that cost 22.8 ms a frame on the A1200, package
 * m7-6). The region is drawn into the texture level as a textured quad
 * through a framebuffer object of the context's own (fbo.c), as any render
 * to texture: the CPU touches no pixel, the copy's job runs after the jobs
 * before it and before those that sample the level, and the level's RAM
 * copy is read back only when the CPU needs it (texture.c's rendered
 * levels).
 *  - From the default framebuffer: its job is rendered and finished (the
 *    picture is then in the render bitmap, top row first), the TFU tiles
 *    the region into a scratch texture (texture.c), and the quad samples it
 *    upside down, red and blue swapped from a BGRA bitmap. The TMU samples
 *    no raster image, and the tile buffer's Flip Y crashed the machine
 *    (plan 7.11), hence the TFU and the flipped coordinates.
 *  - From a framebuffer object: its colour image (a 2D texture's level 0,
 *    or a colour renderbuffer's own texture) sampled directly, nearest and
 *    clamped for the draw; the object's open job renders first (the copy
 *    draws into another framebuffer).
 * The rest goes through the CPU as before (texture.c): a level no render
 * target can hold (LUMINANCE, ALPHA, LUMINANCE_ALPHA), a cube face or a
 * mip level as the source, a texture copied into itself.
 */
#include "ogles2_internal.h"

BOOL ogles2_copy_gpu(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, int level, GLint xoffset,
                     GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    struct OGLES2Framebuffer *saved = ctx->draw_fb, *fb;
    struct OGLES2Texture *src = 0;
    struct OGLES2TexParams params;
    struct OGLES2Surface surf;
    LONG x0, y0, x1, y1, cw, ch;
    GLfloat tc[4];
    BOOL ok;

    if (!ctx->v3d || (ctx->probe & OGLES2_PROBE_CPU_COPY) || !ogles2_texture_renderable(t, k, level))
        return FALSE;
    ogles2_fb_update(ctx);
    /* Only what lies inside the framebuffer: the rest of the region gives
     * undefined texels (GLES 2.0 3.7.2), left as they are. */
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + width > (LONG)ctx->fb_w ? (LONG)ctx->fb_w : x + width;
    y1 = y + height > (LONG)ctx->fb_h ? (LONG)ctx->fb_h : y + height;
    if (x0 >= x1 || y0 >= y1)
        return TRUE;
    cw = x1 - x0;
    ch = y1 - y0;

    if (saved) {
        UWORD sw, sh;
        GLenum f, ty;
        int sk, sl;
        if (!ogles2_fbo_color_image(ctx, &src, &sk, &sl) || src == t || sk || sl ||
            ogles2_texture_target(src) != GL_TEXTURE_2D || !ogles2_texture_image(src, 0, 0, &sw, &sh, &f, &ty))
            return FALSE;
        tc[0] = (GLfloat)x0 / sw;
        tc[1] = (GLfloat)y0 / sh;
        tc[2] = (GLfloat)x1 / sw;
        tc[3] = (GLfloat)y1 / sh;
    } else {
        struct OGLES2Display *d = &ctx->display;
        if (!d->bitmap)
            return FALSE;
        ogles2_job_flush(ctx, FALSE);
        ogles2_job_finish(ctx);
        if (!ogles2_display_lock(d))
            return FALSE;
        /* GL's row y is the bitmap's row height - 1 - y. The GPU draws into
         * the bitmap, so it reaches it (no device-tree check). */
        if (d->bprow >= (ULONG)d->width * 4 && !(d->base & 3))
            src = ogles2_texture_scratch(ctx, (UWORD)cw, (UWORD)ch,
                                         d->base + (ULONG)(d->height - y1) * d->bprow + (ULONG)x0 * 4, d->bprow,
                                         d->rb_swap != 0);
        ogles2_display_unlock(d);
        if (!src)
            return FALSE;
        tc[0] = 0.0f; /* the scratch texture's row 0 is the region's top */
        tc[1] = 1.0f;
        tc[2] = 1.0f;
        tc[3] = 0.0f;
    }

    /* The level as the copy's render target (its GPU copy made first). */
    if (!ogles2_texture_surface(ctx, t, k, level, &surf) || !(fb = ogles2_fbo_copy_target(ctx, t, k, level)))
        return FALSE;
    ctx->draw_fb = fb;
    ogles2_fb_update(ctx);
    if (saved)
        ogles2_texture_params_copy(src, &params);
    ok = ogles2_draw_copy(ctx, src, tc, xoffset + (x0 - x), yoffset + (y0 - y), cw, ch);
    if (saved)
        ogles2_texture_params_restore(src, &params);
    ctx->draw_fb = saved;
    ogles2_fb_update(ctx);
    if (ok && !(ctx->copy_logged & (saved ? 2 : 1))) {
        ctx->copy_logged |= saved ? 2 : 1;
        ogles2_log("glCopyTexSubImage2D: copies from %s drawn by the GPU (the first: %ldx%ld)",
                   (ULONG)(saved ? "a framebuffer object" : "the default framebuffer"), cw, ch);
    }
    return ok;
}
