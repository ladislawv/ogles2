/*
 * Framebuffer and renderbuffer objects (plan section 7.7, M5): names,
 * binding, attachments, completeness (GLES 2.0 4.4.5) and queries, and what
 * a job needs to render into one (ogles2_fbo_target) or glReadPixels to read
 * from one.
 *
 * - Colour images: a texture level (or cube face), or a colour renderbuffer,
 *   whose storage is a texture of its own (texture.c, ogles2_texture_hidden).
 *   The job stores straight into the level's own layout (Mesa's
 *   store_general), so the result is sampled as it is; the texture's RAM
 *   copy is read back only when something needs it (texture.c, sync).
 * - Depth and stencil: renderbuffers, DEPTH_COMPONENT16, STENCIL_INDEX8 and,
 *   OES_packed_depth_stencil, DEPTH24_STENCIL8. Only the GPU reads them, so
 *   they are laid out as the GPU likes, UIF without XOR, padded as Mesa pads
 *   a UIF level (job.c's depth save does the same). Every job stores them and
 *   the next loads them. And depth textures (OES_depth_texture; a
 *   DEPTH_STENCIL_OES one for both): the job stores into the level's own
 *   layout, as for colour, and the TMU samples it from there.
 * - No Y flip: GL's row 0 is the image's row 0, which is what sampling the
 *   texture later expects (plan 7.11).
 * - Jobs, v1: one open job per context; drawing into another framebuffer
 *   renders the open one first (job.c, ogles2_job_begin).
 * - Deleting a texture or renderbuffer detaches it from every framebuffer
 *   object of the context (GLES2 asks this only of the bound one; the others
 *   would keep an image whose memory is gone).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_frame.h"
#include "v3d_hw.h"

#ifndef GL_RGB8_OES
#define GL_RGB8_OES  0x8051 /* OES_rgb8_rgba8 */
#define GL_RGBA8_OES 0x8058
#endif
#ifndef GL_DEPTH24_STENCIL8_OES
#define GL_DEPTH24_STENCIL8_OES 0x88F0 /* OES_packed_depth_stencil */
#endif

#define ATT_COLOR   0
#define ATT_DEPTH   1
#define ATT_STENCIL 2

struct OGLES2Renderbuffer
{
    GLuint name;
    BOOL bound_once;
    GLenum format;              /* GL_RGBA4 until storage is given (GLES 2.0 table 6.23) */
    UWORD w, h;
    struct OGLES2Texture *color; /* a colour format's storage */
    v3d_mem mem;                /* a depth or stencil format's, GPU only */
    ULONG base, ub, bytes;      /* 4096-aligned image; padded height in UIF blocks; size */
    UBYTE cpp;
};

struct OGLES2Attachment
{
    GLenum type;                /* GL_NONE, GL_TEXTURE, GL_RENDERBUFFER */
    struct OGLES2Texture *tex;
    UBYTE face, level;
    struct OGLES2Renderbuffer *rb;
};

struct OGLES2Framebuffer
{
    GLuint name;
    BOOL bound_once;
    UBYTE discarded;            /* OGLES2_DISCARD_*: undefined, the next job need not load them */
    struct OGLES2Attachment att[3];
    v3d_mem pool_alloc, pool_state; /* tile pools of its own when bigger than the context's */
    ULONG pool_tiles;
};

struct OGLES2FboState
{
    struct OGLES2Framebuffer **fbs;  /* the context's own (GLES 2.0: not shared) */
    GLuint nfbs;
    struct OGLES2Share *sh;          /* the renderbuffers: sh->rbs, sh->nrbs (share.c) */
    struct OGLES2Renderbuffer *rb_bound;
    BOOL warned_incomplete;
};

static struct OGLES2FboState *fs_get(struct OGLES2Context *ctx)
{
    if (!ctx->fbo && (ctx->fbo = AllocVec(sizeof(struct OGLES2FboState), MEMF_ANY | MEMF_CLEAR)))
        ctx->fbo->sh = ctx->share;
    return ctx->fbo;
}

/* Makes `name` an index of a name table of `*n` pointers. */
static BOOL table_room(void ***tab, GLuint *n, GLuint name)
{
    GLuint nn, j;
    void **t;
    if (name < *n)
        return TRUE;
    nn = *n ? *n : 16;
    while (nn <= name)
        nn *= 2;
    if (!(t = AllocVec(nn * sizeof(void *), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    for (j = 0; j < *n; j++)
        t[j] = (*tab)[j];
    if (*tab)
        FreeVec(*tab);
    *tab = t;
    *n = nn;
    return TRUE;
}

static struct OGLES2Framebuffer *fb_by_name(struct OGLES2FboState *fs, GLuint name)
{
    return name && name < fs->nfbs ? fs->fbs[name] : 0;
}

static struct OGLES2Renderbuffer *rb_by_name(struct OGLES2FboState *fs, GLuint name)
{
    return name && name < fs->sh->nrbs ? fs->sh->rbs[name] : 0;
}

/* ---- renderbuffer storage ---- */

static void rb_free_storage(struct OGLES2Context *ctx, struct OGLES2Renderbuffer *rb)
{
    if (rb->color)
        ogles2_texture_hidden_free(ctx, rb->color);
    rb->color = 0;
    if (rb->mem.hostptr)
        v3d_mem_free(ogles2_device(), &rb->mem);
    memset(&rb->mem, 0, sizeof(rb->mem));
    rb->base = rb->ub = rb->bytes = 0;
}

static BOOL is_depth(GLenum f)
{
    return f == GL_DEPTH_COMPONENT16 || f == GL_DEPTH24_STENCIL8_OES;
}

static BOOL is_stencil(GLenum f)
{
    return f == GL_STENCIL_INDEX8 || f == GL_DEPTH24_STENCIL8_OES;
}

static BOOL is_color(GLenum f)
{
    return f == GL_RGBA4 || f == GL_RGB5_A1 || f == GL_RGB565 || f == GL_RGB8_OES || f == GL_RGBA8_OES;
}

/* Depth and stencil: UIF without XOR, Mesa's padding for a UIF level. */
static BOOL rb_zs_storage(struct OGLES2Renderbuffer *rb)
{
    ULONG cpp = rb->format == GL_DEPTH_COMPONENT16 ? 2 : rb->format == GL_STENCIL_INDEX8 ? 1 : 4;
    ULONG ub_w = 2 * v3d_utile_width((int)cpp), ub_h = 2 * v3d_utile_height((int)cpp);
    ULONG pw = (rb->w + 4 * ub_w - 1) / (4 * ub_w) * (4 * ub_w);
    ULONG ph = (rb->h + ub_h - 1) / ub_h * ub_h;
    if (v3d_mem_alloc(ogles2_device(), &rb->mem, pw * ph * cpp + 4096) < 0)
        return FALSE;
    rb->base = ((ULONG)rb->mem.hostptr + 4095) & ~4095UL;
    rb->ub = ph / ub_h;
    rb->bytes = pw * ph * cpp;
    rb->cpp = (UBYTE)cpp;
    return TRUE;
}

/* ---- the job of the framebuffer an attachment change touches ---- */

/* The open job draws into fb: render it before fb changes; and a running
 * render into an object finishes first (M6 overlap). */
static void before_fb_change(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb)
{
    if (ctx->v3d && ctx->v3d->frame_active && ctx->job_fb == fb && fb)
        ogles2_job_flush(ctx, FALSE);
    if (ctx->pend && ctx->pend_fb)
        ogles2_job_finish(ctx);
}

/* The open job draws into a framebuffer that uses rb. */
static void before_rb_change(struct OGLES2Context *ctx, struct OGLES2Renderbuffer *rb)
{
    struct OGLES2Framebuffer *fb = ctx->job_fb;
    int a;
    if (ctx->pend && ctx->pend_fb)
        ogles2_job_finish(ctx);
    if (!ctx->v3d || !ctx->v3d->frame_active || !fb)
        return;
    for (a = 0; a < 3; a++)
        if (fb->att[a].type == GL_RENDERBUFFER && fb->att[a].rb == rb) {
            ogles2_job_flush(ctx, FALSE);
            ogles2_job_finish(ctx);
            return;
        }
}

/* fb draws into texture t: its colour image (a renderbuffer's own
 * included), or a depth texture. */
static BOOL draws_into(const struct OGLES2Framebuffer *fb, const struct OGLES2Texture *t)
{
    return fb && ((fb->att[ATT_COLOR].type == GL_TEXTURE && fb->att[ATT_COLOR].tex == t) ||
                  (fb->att[ATT_DEPTH].type == GL_TEXTURE && fb->att[ATT_DEPTH].tex == t) ||
                  (fb->att[ATT_STENCIL].type == GL_TEXTURE && fb->att[ATT_STENCIL].tex == t) ||
                  (fb->att[ATT_COLOR].type == GL_RENDERBUFFER && fb->att[ATT_COLOR].rb &&
                   fb->att[ATT_COLOR].rb->color == t));
}

void ogles2_fbo_texture_changing(struct OGLES2Context *ctx, struct OGLES2Texture *t)
{
    if (ctx->v3d && ctx->v3d->frame_active && draws_into(ctx->job_fb, t))
        ogles2_job_flush(ctx, FALSE);
    if (ctx->pend && draws_into(ctx->pend_fb, t)) /* the running render writes it */
        ogles2_job_finish(ctx);
}

void ogles2_fbo_texture_deleted(struct OGLES2Context *ctx, struct OGLES2Texture *t)
{
    struct OGLES2FboState *fs = ctx->fbo;
    GLuint i;
    int a;
    if (!fs)
        return;
    ogles2_fbo_texture_changing(ctx, t);
    for (i = 1; i < fs->nfbs; i++)
        if (fs->fbs[i])
            for (a = 0; a < 3; a++)
                if (fs->fbs[i]->att[a].type == GL_TEXTURE && fs->fbs[i]->att[a].tex == t)
                    memset(&fs->fbs[i]->att[a], 0, sizeof(fs->fbs[i]->att[a]));
}

/* ---- names and binding ---- */

void ogles2_glGenFramebuffers(struct OGLES2Context *ctx, GLsizei n, GLuint *framebuffers)
{
    struct OGLES2FboState *fs;
    GLsizei k;
    GLuint i = 1;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(fs = fs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < n; k++) {
        struct OGLES2Framebuffer *fb;
        while (i < fs->nfbs && fs->fbs[i])
            i++;
        if (!table_room((void ***)&fs->fbs, &fs->nfbs, i) ||
            !(fb = AllocVec(sizeof(*fb), MEMF_ANY | MEMF_CLEAR))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        fb->name = i;
        fs->fbs[i] = fb;
        if (framebuffers)
            framebuffers[k] = i;
    }
}

void ogles2_glGenRenderbuffers(struct OGLES2Context *ctx, GLsizei n, GLuint *renderbuffers)
{
    struct OGLES2FboState *fs;
    GLsizei k;
    GLuint i = 1;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(fs = fs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < n; k++) {
        struct OGLES2Renderbuffer *rb;
        while (i < fs->sh->nrbs && fs->sh->rbs[i])
            i++;
        if (!table_room((void ***)&fs->sh->rbs, &fs->sh->nrbs, i) ||
            !(rb = AllocVec(sizeof(*rb), MEMF_ANY | MEMF_CLEAR))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        rb->name = i;
        rb->format = GL_RGBA4;
        fs->sh->rbs[i] = rb;
        if (renderbuffers)
            renderbuffers[k] = i;
    }
}

static void fb_free(struct OGLES2Framebuffer *fb)
{
    if (fb->pool_alloc.hostptr)
        v3d_mem_free(ogles2_device(), &fb->pool_alloc);
    if (fb->pool_state.hostptr)
        v3d_mem_free(ogles2_device(), &fb->pool_state);
    FreeVec(fb);
}

void ogles2_glBindFramebuffer(struct OGLES2Context *ctx, GLenum target, GLuint framebuffer)
{
    struct OGLES2FboState *fs;
    struct OGLES2Framebuffer *fb = 0;
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_FRAMEBUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(fs = fs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (framebuffer) {
        /* GLES2 lets a program bind a name glGenFramebuffers did not return. */
        if (!(fb = fb_by_name(fs, framebuffer))) {
            if (!table_room((void ***)&fs->fbs, &fs->nfbs, framebuffer) ||
                !(fb = AllocVec(sizeof(*fb), MEMF_ANY | MEMF_CLEAR))) {
                ogles2_error(ctx, GL_OUT_OF_MEMORY);
                return;
            }
            fb->name = framebuffer;
            fs->fbs[framebuffer] = fb;
        }
        fb->bound_once = TRUE;
    }
    /* The open job is rendered when something is drawn into the other
     * framebuffer (ogles2_job_begin), not here: binding back and forth
     * without drawing costs nothing. */
    ctx->draw_fb = fb;
    ogles2_fb_update(ctx);
}

void ogles2_glBindRenderbuffer(struct OGLES2Context *ctx, GLenum target, GLuint renderbuffer)
{
    struct OGLES2FboState *fs;
    struct OGLES2Renderbuffer *rb = 0;
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_RENDERBUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(fs = fs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (renderbuffer && !(rb = rb_by_name(fs, renderbuffer))) {
        if (!table_room((void ***)&fs->sh->rbs, &fs->sh->nrbs, renderbuffer) ||
            !(rb = AllocVec(sizeof(*rb), MEMF_ANY | MEMF_CLEAR))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        rb->name = renderbuffer;
        rb->format = GL_RGBA4;
        fs->sh->rbs[renderbuffer] = rb;
    }
    if (rb)
        rb->bound_once = TRUE;
    fs->rb_bound = rb;
}

void ogles2_glDeleteFramebuffers(struct OGLES2Context *ctx, GLsizei n, const GLuint *framebuffers)
{
    struct OGLES2FboState *fs;
    GLsizei k;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(fs = ctx->fbo) || !framebuffers)
        return;
    for (k = 0; k < n; k++) {
        struct OGLES2Framebuffer *fb = fb_by_name(fs, framebuffers[k]);
        if (!fb)
            continue;
        before_fb_change(ctx, fb); /* what was drawn into it is rendered */
        if (ctx->job_fb == fb)
            ctx->job_fb = 0;
        if (ctx->draw_fb == fb) {   /* GLES 2.0 4.4.1: as if 0 were bound */
            ctx->draw_fb = 0;
            ogles2_fb_update(ctx);
        }
        fs->fbs[fb->name] = 0;
        fb_free(fb);
    }
}

void ogles2_glDeleteRenderbuffers(struct OGLES2Context *ctx, GLsizei n, const GLuint *renderbuffers)
{
    struct OGLES2FboState *fs;
    struct OGLES2Context *m;
    GLsizei k;
    GLuint i;
    int a;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(fs = fs_get(ctx)) || !renderbuffers)
        return;
    for (k = 0; k < n; k++) {
        struct OGLES2Renderbuffer *rb = rb_by_name(fs, renderbuffers[k]);
        if (!rb)
            continue;
        /* Every member of the share group lets go of it: its jobs that use
         * it rendered first, its binding and attachments cleared (GL would
         * keep it while attached elsewhere). */
        for (m = ctx->share->first; m; m = m->share_next) {
            struct OGLES2FboState *mfs = m->fbo;
            before_rb_change(m, rb);
            if (!mfs)
                continue;
            if (mfs->rb_bound == rb)
                mfs->rb_bound = 0;
            for (i = 1; i < mfs->nfbs; i++)
                if (mfs->fbs[i])
                    for (a = 0; a < 3; a++)
                        if (mfs->fbs[i]->att[a].type == GL_RENDERBUFFER && mfs->fbs[i]->att[a].rb == rb)
                            memset(&mfs->fbs[i]->att[a], 0, sizeof(mfs->fbs[i]->att[a]));
        }
        fs->sh->rbs[rb->name] = 0;
        rb_free_storage(ctx, rb);
        FreeVec(rb);
    }
    for (m = ctx->share->first; m; m = m->share_next)
        ogles2_fb_update(m);
}

GLboolean ogles2_glIsFramebuffer(struct OGLES2Context *ctx, GLuint framebuffer)
{
    struct OGLES2Framebuffer *fb;
    if (!ogles2_context_valid(ctx) || !ctx->fbo)
        return GL_FALSE;
    fb = fb_by_name(ctx->fbo, framebuffer);
    return fb && fb->bound_once ? GL_TRUE : GL_FALSE;
}

GLboolean ogles2_glIsRenderbuffer(struct OGLES2Context *ctx, GLuint renderbuffer)
{
    struct OGLES2Renderbuffer *rb;
    if (!ogles2_context_valid(ctx) || !ctx->fbo)
        return GL_FALSE;
    rb = rb_by_name(ctx->fbo, renderbuffer);
    return rb && rb->bound_once ? GL_TRUE : GL_FALSE;
}

void ogles2_glRenderbufferStorage(struct OGLES2Context *ctx, GLenum target, GLenum internalformat,
                                  GLsizei width, GLsizei height)
{
    struct OGLES2FboState *fs;
    struct OGLES2Renderbuffer *rb;
    BOOL ok = TRUE;
    if (!ogles2_context_valid(ctx) || !(fs = fs_get(ctx)))
        return;
    if (target != GL_RENDERBUFFER ||
        !(is_color(internalformat) || is_depth(internalformat) || is_stencil(internalformat))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0 || width > OGLES2_MAX_VIEWPORT || height > OGLES2_MAX_VIEWPORT) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(rb = fs->rb_bound)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    {
        struct OGLES2Context *m; /* any member's job that uses it, first */
        for (m = ctx->share->first; m; m = m->share_next)
            before_rb_change(m, rb);
    }
    rb_free_storage(ctx, rb);
    rb->format = internalformat;
    rb->w = (UWORD)width;
    rb->h = (UWORD)height;
    if (width && height && ctx->v3d) {
        if (is_color(internalformat)) {
            GLenum f = internalformat == GL_RGB565 || internalformat == GL_RGB8_OES ? GL_RGB : GL_RGBA;
            GLenum ty = internalformat == GL_RGBA4    ? GL_UNSIGNED_SHORT_4_4_4_4
                      : internalformat == GL_RGB5_A1  ? GL_UNSIGNED_SHORT_5_5_5_1
                      : internalformat == GL_RGB565   ? GL_UNSIGNED_SHORT_5_6_5
                                                      : GL_UNSIGNED_BYTE;
            ok = (rb->color = ogles2_texture_hidden(ctx, (UWORD)width, (UWORD)height, f, ty)) != 0;
        } else {
            ok = rb_zs_storage(rb);
        }
    }
    if (!ok) {
        rb->w = rb->h = 0;
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
    }
    ogles2_fb_update(ctx);
}

/* ---- attachments ---- */

static int attachment_index(GLenum attachment)
{
    return attachment == GL_COLOR_ATTACHMENT0 ? ATT_COLOR
         : attachment == GL_DEPTH_ATTACHMENT  ? ATT_DEPTH
         : attachment == GL_STENCIL_ATTACHMENT ? ATT_STENCIL : -1;
}

void ogles2_glFramebufferRenderbuffer(struct OGLES2Context *ctx, GLenum target, GLenum attachment,
                                      GLenum renderbuffertarget, GLuint renderbuffer)
{
    struct OGLES2FboState *fs;
    struct OGLES2Renderbuffer *rb = 0;
    struct OGLES2Attachment *at;
    int a = attachment_index(attachment);
    if (!ogles2_context_valid(ctx) || !(fs = fs_get(ctx)))
        return;
    if (target != GL_FRAMEBUFFER || a < 0 || (renderbuffer && renderbuffertarget != GL_RENDERBUFFER)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* A name is an object once bound (GLES 2.0 4.4.3). */
    if (!ctx->draw_fb || (renderbuffer && (!(rb = rb_by_name(fs, renderbuffer)) || !rb->bound_once))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    before_fb_change(ctx, ctx->draw_fb);
    at = &ctx->draw_fb->att[a];
    memset(at, 0, sizeof(*at));
    if (rb) {
        at->type = GL_RENDERBUFFER;
        at->rb = rb;
    }
    ogles2_fb_update(ctx);
}

void ogles2_glFramebufferTexture2D(struct OGLES2Context *ctx, GLenum target, GLenum attachment,
                                   GLenum textarget, GLuint texture, GLint level)
{
    struct OGLES2Texture *t = 0;
    struct OGLES2Attachment *at;
    int a = attachment_index(attachment);
    BOOL cube_face = textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
    if (!ogles2_context_valid(ctx) || !fs_get(ctx))
        return;
    if (target != GL_FRAMEBUFFER || a < 0 ||
        (texture && textarget != GL_TEXTURE_2D && textarget != GL_TEXTURE_RECTANGLE && !cube_face)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!ctx->draw_fb) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (texture) {
        GLenum tt;
        /* GLES 2.0 4.4.3 takes level 0 only; OES_fbo_render_mipmap (offered,
         * as Mesa does) any level. */
        if (level < 0 || level > 12 || (textarget == GL_TEXTURE_RECTANGLE && level != 0)) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return;
        }
        /* The texture's own target: 2D, a rectangle (ARB_texture_rectangle),
         * or a face for a cube map. */
        if (!(t = ogles2_texture_get(ctx, texture)) || !(tt = ogles2_texture_target(t)) ||
            (cube_face ? tt != GL_TEXTURE_CUBE_MAP : tt != textarget)) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    }
    before_fb_change(ctx, ctx->draw_fb);
    at = &ctx->draw_fb->att[a];
    memset(at, 0, sizeof(*at));
    if (t) {
        at->type = GL_TEXTURE;
        at->tex = t;
        at->face = cube_face ? (UBYTE)(textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
        at->level = (UBYTE)level;
    }
    ogles2_fb_update(ctx);
}

/* ---- EXT_discard_framebuffer ----
 * The images' contents become undefined: the open job of that framebuffer
 * does not store them, and its next job does not load them (Mesa: the
 * job's store and load masks). The default framebuffer: depth and stencil
 * are then not kept across a mid-frame flush; its colour is always kept. */
void ogles2_glDiscardFramebufferEXT(struct OGLES2Context *ctx, GLenum target, GLsizei numAttachments,
                                    const GLenum *attachments)
{
    UBYTE bits = 0;
    GLsizei k;
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_FRAMEBUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (numAttachments < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (k = 0; k < numAttachments && attachments; k++) {
        GLenum a = attachments[k];
        if (ctx->draw_fb ? a == GL_COLOR_ATTACHMENT0 : a == 0x1800 /* GL_COLOR_EXT */)
            bits |= OGLES2_DISCARD_COLOR;
        else if (ctx->draw_fb ? a == GL_DEPTH_ATTACHMENT : a == 0x1801 /* GL_DEPTH_EXT */)
            bits |= OGLES2_DISCARD_DEPTH;
        else if (ctx->draw_fb ? a == GL_STENCIL_ATTACHMENT : a == 0x1802 /* GL_STENCIL_EXT */)
            bits |= OGLES2_DISCARD_STENCIL;
        else {
            ogles2_error(ctx, GL_INVALID_ENUM);
            return;
        }
    }
    if (!ctx->draw_fb) {
        if (bits & (OGLES2_DISCARD_DEPTH | OGLES2_DISCARD_STENCIL)) {
            if (ctx->v3d && ctx->v3d->frame_active && !ctx->job_fb)
                ctx->job_discard |= OGLES2_DISCARD_DEPTH | OGLES2_DISCARD_STENCIL;
            else
                ctx->zs_saved = FALSE;
        }
        return;
    }
    if (ctx->v3d && ctx->v3d->frame_active && ctx->job_fb == ctx->draw_fb)
        ctx->job_discard |= bits;
    ctx->draw_fb->discarded |= bits;
}

/* job.c, at a job's start: what it need not load. */
UBYTE ogles2_fbo_take_discarded(struct OGLES2Framebuffer *fb)
{
    UBYTE b = fb->discarded;
    fb->discarded = 0;
    return b;
}

/* ---- completeness (GLES 2.0 4.4.5) ---- */

/* An attachment's image size; FALSE if it has none. */
static BOOL att_size(const struct OGLES2Attachment *at, UWORD *w, UWORD *h)
{
    GLenum f, ty;
    if (at->type == GL_RENDERBUFFER) {
        *w = at->rb->w;
        *h = at->rb->h;
        return TRUE;
    }
    return ogles2_texture_image(at->tex, at->face, at->level, w, h, &f, &ty);
}

/* An image with depth and stencil in one: a D24S8 renderbuffer, or a
 * DEPTH_STENCIL_OES texture. */
static BOOL packed(const struct OGLES2Attachment *at)
{
    UBYTE bits;
    BOOL st = FALSE;
    if (at->type == GL_RENDERBUFFER)
        return at->rb->format == GL_DEPTH24_STENCIL8_OES;
    return at->type == GL_TEXTURE && ogles2_texture_depth(at->tex, at->level, &bits, &st) && st;
}

static BOOL same_image(const struct OGLES2Attachment *a, const struct OGLES2Attachment *b)
{
    return a->type == b->type &&
           (a->type == GL_RENDERBUFFER ? a->rb == b->rb : a->tex == b->tex && a->level == b->level);
}

static GLenum status_of(struct OGLES2Framebuffer *fb)
{
    const struct OGLES2Attachment *c = &fb->att[ATT_COLOR], *d = &fb->att[ATT_DEPTH],
                                  *s = &fb->att[ATT_STENCIL];
    UWORD w0 = 0, h0 = 0, w, h;
    int a, n = 0;
    for (a = 0; a < 3; a++) {
        const struct OGLES2Attachment *at = &fb->att[a];
        if (at->type == GL_NONE)
            continue;
        if (!att_size(at, &w, &h) || !w || !h)
            return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (at->type == GL_RENDERBUFFER) {
            GLenum f = at->rb->format;
            if ((a == ATT_COLOR && !is_color(f)) || (a == ATT_DEPTH && !is_depth(f)) ||
                (a == ATT_STENCIL && !is_stencil(f)))
                return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        } else if (a == ATT_COLOR) {
            if (!ogles2_texture_renderable(at->tex, at->face, at->level))
                return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        } else {
            /* OES_depth_texture: a depth image for the depth, and, with
             * OES_packed_depth_stencil, a depth-stencil one for the stencil. */
            UBYTE bits;
            BOOL st;
            if (!ogles2_texture_depth(at->tex, at->level, &bits, &st) || (a == ATT_STENCIL && !st))
                return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        }
        if (n++ == 0) {
            w0 = w;
            h0 = h;
        } else if (w != w0 || h != h0) {
            return GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS;
        }
    }
    if (!n)
        return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    /* Depth and stencil: one packed image for both, or apart without a
     * packed one (D16 or a depth texture, and S8). */
    if (d->type != GL_NONE && s->type != GL_NONE && (packed(d) || packed(s)) && !same_image(d, s))
        return GL_FRAMEBUFFER_UNSUPPORTED;
    (void)c;
    return GL_FRAMEBUFFER_COMPLETE;
}

GLenum ogles2_glCheckFramebufferStatus(struct OGLES2Context *ctx, GLenum target)
{
    if (!ogles2_context_valid(ctx))
        return 0;
    if (target != GL_FRAMEBUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    return ogles2_fb_status(ctx);
}

GLenum ogles2_fb_status(struct OGLES2Context *ctx)
{
    return ctx->draw_fb ? status_of(ctx->draw_fb) : GL_FRAMEBUFFER_COMPLETE;
}

/* The fb_* fields: the default framebuffer's, or the bound object's. */
void ogles2_fb_update(struct OGLES2Context *ctx)
{
    struct OGLES2Framebuffer *fb = ctx->draw_fb;
    int a;
    if (!fb) {
        ctx->fb_w = (UWORD)ctx->width;
        ctx->fb_h = (UWORD)ctx->height;
        ctx->fb_flip = TRUE;
        ctx->fb_depth_bits = ctx->depth_bits;
        ctx->fb_stencil_bits = ctx->stencil_bits;
        ctx->fb_depth_format = ctx->depth_format;
        return;
    }
    ctx->fb_w = ctx->fb_h = 0;
    ctx->fb_flip = FALSE;
    ctx->fb_depth_bits = ctx->fb_stencil_bits = 0;
    ctx->fb_depth_format = OGLES2_DEPTH_16;
    for (a = 0; a < 3; a++) {
        UWORD w, h;
        if (fb->att[a].type != GL_NONE && !ctx->fb_w && att_size(&fb->att[a], &w, &h)) {
            ctx->fb_w = w;
            ctx->fb_h = h;
        }
    }
    if (fb->att[ATT_DEPTH].type == GL_RENDERBUFFER) {
        GLenum f = fb->att[ATT_DEPTH].rb->format;
        if (f == GL_DEPTH_COMPONENT16) {
            ctx->fb_depth_bits = 16;
        } else if (f == GL_DEPTH24_STENCIL8_OES) {
            ctx->fb_depth_bits = 24;
            ctx->fb_depth_format = OGLES2_DEPTH_24;
        }
    }
    if (fb->att[ATT_STENCIL].type == GL_RENDERBUFFER && is_stencil(fb->att[ATT_STENCIL].rb->format)) {
        ctx->fb_stencil_bits = 8;
        if (fb->att[ATT_STENCIL].rb->format == GL_DEPTH24_STENCIL8_OES)
            ctx->fb_depth_format = OGLES2_DEPTH_24;
    }
    /* OES_depth_texture: a 16-bit depth texture is D16, a 24-bit one (and
     * a depth-stencil one) D24S8 in the tile buffer. */
    {
        UBYTE bits;
        BOOL st;
        const struct OGLES2Attachment *d = &fb->att[ATT_DEPTH], *s = &fb->att[ATT_STENCIL];
        if (d->type == GL_TEXTURE && ogles2_texture_depth(d->tex, d->level, &bits, &st)) {
            ctx->fb_depth_bits = bits;
            if (bits == 24)
                ctx->fb_depth_format = OGLES2_DEPTH_24;
        }
        if (s->type == GL_TEXTURE && ogles2_texture_depth(s->tex, s->level, &bits, &st) && st) {
            ctx->fb_stencil_bits = 8;
            ctx->fb_depth_format = OGLES2_DEPTH_24;
        }
    }
}

BOOL ogles2_fb_ready(struct OGLES2Context *ctx)
{
    GLenum st;
    ogles2_fb_update(ctx);
    if (!ctx->draw_fb || (st = status_of(ctx->draw_fb)) == GL_FRAMEBUFFER_COMPLETE)
        return TRUE;
    ogles2_error(ctx, GL_INVALID_FRAMEBUFFER_OPERATION);
    if (ctx->fbo && !ctx->fbo->warned_incomplete) {
        ctx->fbo->warned_incomplete = TRUE;
        ogles2_log("framebuffer %ld is not complete (0x%04lx): nothing drawn or read (logged once)",
                   (ULONG)ctx->draw_fb->name, (ULONG)st);
    }
    return FALSE;
}

/* ---- the job's target (job.c) ---- */

static void rb_surface(const struct OGLES2Renderbuffer *rb, BOOL stencil_only, struct OGLES2Surface *s)
{
    s->addr = rb->base;
    s->ub = rb->ub;
    s->bytes = rb->bytes;
    s->mem_format = V3D_MEMORY_FORMAT_UIF_NO_XOR;
    s->format = stencil_only                         ? V3D_OUTPUT_IMAGE_FORMAT_S8
              : rb->format == GL_DEPTH_COMPONENT16 ? V3D_OUTPUT_IMAGE_FORMAT_D16
                                                   : V3D_OUTPUT_IMAGE_FORMAT_D24S8;
    s->force_alpha1 = FALSE;
}

/* Tile pools for w x h: the context's, if it has as many tiles; else fb's. */
static BOOL pools(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb, struct OGLES2Target *t)
{
    V3DContext *v = ctx->v3d;
    v3d_u32 zs, ts, ta, tx, ty;
    ULONG tiles = (ULONG)((t->w + 63) / 64) * ((t->h + 63) / 64);
    ULONG mine = (ULONG)((v->width + 63) / 64) * ((v->height + 63) / 64);
    t->ta_addr = t->ta_size = t->ts_addr = 0;
    if (tiles <= mine)
        return TRUE;
    if (fb->pool_tiles < tiles) {
        ogles2_job_finish(ctx); /* a running render may use the old pools */
        if (fb->pool_alloc.hostptr)
            v3d_mem_free(ogles2_device(), &fb->pool_alloc);
        if (fb->pool_state.hostptr)
            v3d_mem_free(ogles2_device(), &fb->pool_state);
        fb->pool_tiles = 0;
        v3d_frame_compute_pool_sizes(t->w, t->h, 16, 0, &zs, &ts, &ta, &tx, &ty);
        if (v3d_mem_alloc(ogles2_device(), &fb->pool_alloc, ta + 4096) < 0)
            return FALSE;
        if (v3d_mem_alloc(ogles2_device(), &fb->pool_state, ts + 4096) < 0) {
            v3d_mem_free(ogles2_device(), &fb->pool_alloc);
            return FALSE;
        }
        {
            ULONG len = (ULONG)fb->pool_alloc.size;
            CachePreDMA(fb->pool_alloc.hostptr, &len, 0);
            len = (ULONG)fb->pool_state.size;
            CachePreDMA(fb->pool_state.hostptr, &len, 0);
        }
        fb->pool_tiles = tiles;
        ogles2_log("framebuffer %ld: tile pools of its own for %ldx%ld", (ULONG)fb->name, (ULONG)t->w,
                   (ULONG)t->h);
    }
    t->ta_addr = ((ULONG)fb->pool_alloc.hostptr + 4095) & ~4095UL;
    t->ta_size = (ULONG)fb->pool_alloc.size - (t->ta_addr - (ULONG)fb->pool_alloc.hostptr);
    t->ts_addr = ((ULONG)fb->pool_state.hostptr + 4095) & ~4095UL;
    return TRUE;
}

BOOL ogles2_fbo_target(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb, BOOL surfaces,
                       struct OGLES2Target *t)
{
    const struct OGLES2Attachment *c = &fb->att[ATT_COLOR], *d = &fb->att[ATT_DEPTH],
                                  *s = &fb->att[ATT_STENCIL];
    struct OGLES2Framebuffer *saved = ctx->draw_fb;
    memset(t, 0, sizeof(*t));
    ctx->draw_fb = fb; /* the sizes and formats of fb, whatever is bound now */
    ogles2_fb_update(ctx);
    t->w = ctx->fb_w;
    t->h = ctx->fb_h;
    t->depth_type = ctx->fb_depth_format == OGLES2_DEPTH_24 ? V3D_INTERNAL_TYPE_DEPTH24
                                                             : V3D_INTERNAL_TYPE_DEPTH16;
    ctx->draw_fb = saved;
    ogles2_fb_update(ctx);
    if (!t->w || !t->h || !pools(ctx, fb, t))
        return FALSE;
    if (!surfaces)
        return TRUE;
    if (c->type == GL_TEXTURE) {
        if (!ogles2_texture_surface(ctx, c->tex, c->face, c->level, &t->color))
            return FALSE;
        t->has_color = TRUE;
        t->tex = c->tex;
        t->face = c->face;
        t->level = c->level;
    } else if (c->type == GL_RENDERBUFFER) {
        if (!c->rb->color || !ogles2_texture_surface(ctx, c->rb->color, 0, 0, &t->color))
            return FALSE;
        t->has_color = TRUE;
        t->tex = c->rb->color;
    }
    if (d->type == GL_RENDERBUFFER && d->rb->base) {
        t->has_z = TRUE;
        rb_surface(d->rb, FALSE, &t->z);
        t->packed_zs = d->rb->format == GL_DEPTH24_STENCIL8_OES;
    }
    if (s->type == GL_RENDERBUFFER && s->rb->base && !(t->packed_zs && s->rb == d->rb)) {
        if (s->rb->format == GL_DEPTH24_STENCIL8_OES) { /* packed, as stencil only */
            t->has_z = TRUE;
            t->packed_zs = TRUE;
            rb_surface(s->rb, FALSE, &t->z);
        } else {
            t->has_s = TRUE;
            rb_surface(s->rb, TRUE, &t->s);
        }
    }
    /* OES_depth_texture: the depth texture's level itself; a depth-stencil
     * texture for both is one packed image, as a D24S8 renderbuffer. */
    if (d->type == GL_TEXTURE) {
        if (!ogles2_texture_zs_surface(ctx, d->tex, d->level, &t->z))
            return FALSE;
        t->has_z = TRUE;
        t->packed_zs = same_image(d, s);
    }
    if (s->type == GL_TEXTURE && !t->packed_zs) { /* a depth-stencil texture as the stencil only */
        if (!ogles2_texture_zs_surface(ctx, s->tex, s->level, &t->z))
            return FALSE;
        t->has_z = TRUE;
        t->packed_zs = TRUE;
    }
    return TRUE;
}

void ogles2_fbo_rendered(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb)
{
    const struct OGLES2Attachment *c = &fb->att[ATT_COLOR];
    int a;
    (void)ctx;
    if (c->type == GL_TEXTURE)
        ogles2_texture_rendered(c->tex, c->face, c->level);
    else if (c->type == GL_RENDERBUFFER && c->rb->color)
        ogles2_texture_rendered(c->rb->color, 0, 0);
    for (a = ATT_DEPTH; a <= ATT_STENCIL; a++) /* depth textures (OES_depth_texture) */
        if (fb->att[a].type == GL_TEXTURE)
            ogles2_texture_rendered(fb->att[a].tex, 0, fb->att[a].level);
}

BOOL ogles2_fbo_read(struct OGLES2Context *ctx, int x, int y, int w, int h, UBYTE *rgba, ULONG stride)
{
    const struct OGLES2Attachment *c = &ctx->draw_fb->att[ATT_COLOR];
    if (c->type == GL_TEXTURE)
        return ogles2_texture_read(c->tex, c->face, c->level, x, y, w, h, rgba, stride);
    if (c->type == GL_RENDERBUFFER && c->rb->color)
        return ogles2_texture_read(c->rb->color, 0, 0, x, y, w, h, rgba, stride);
    return FALSE;
}

/* ---- queries ---- */

static void rb_bits(GLenum f, GLint *r, GLint *g, GLint *b, GLint *a, GLint *d, GLint *s)
{
    *r = *g = *b = *a = *d = *s = 0;
    switch (f) {
    case GL_RGBA4:   *r = *g = *b = *a = 4; break;
    case GL_RGB5_A1: *r = *g = *b = 5; *a = 1; break;
    case GL_RGB565:  *r = *b = 5; *g = 6; break;
    case GL_RGB8_OES: *r = *g = *b = 8; break;
    case GL_RGBA8_OES: *r = *g = *b = *a = 8; break;
    case GL_DEPTH_COMPONENT16: *d = 16; break;
    case GL_STENCIL_INDEX8: *s = 8; break;
    case GL_DEPTH24_STENCIL8_OES: *d = 24; *s = 8; break;
    }
}

/* A texture image's colour bits. */
static void tex_bits(const struct OGLES2Attachment *at, GLint *r, GLint *g, GLint *b, GLint *a)
{
    UWORD w, h;
    GLenum f = 0, ty = 0;
    ogles2_texture_image(at->tex, at->face, at->level, &w, &h, &f, &ty);
    *r = *g = *b = 8;
    *a = f == GL_RGBA ? 8 : 0;
    if (ty == GL_UNSIGNED_SHORT_5_6_5) { *r = *b = 5; *g = 6; }
    if (ty == GL_UNSIGNED_SHORT_4_4_4_4) { *r = *g = *b = *a = 4; }
    if (ty == GL_UNSIGNED_SHORT_5_5_5_1) { *r = *g = *b = 5; *a = 1; }
}

void ogles2_glGetRenderbufferParameteriv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                         GLint *params)
{
    struct OGLES2Renderbuffer *rb;
    GLint bits[6];
    if (!ogles2_context_valid(ctx) || !params || !fs_get(ctx))
        return;
    if (target != GL_RENDERBUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(rb = ctx->fbo->rb_bound)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    rb_bits(rb->w ? rb->format : 0, &bits[0], &bits[1], &bits[2], &bits[3], &bits[4], &bits[5]);
    switch (pname) {
    case GL_RENDERBUFFER_WIDTH:           *params = rb->w; break;
    case GL_RENDERBUFFER_HEIGHT:          *params = rb->h; break;
    case GL_RENDERBUFFER_INTERNAL_FORMAT: *params = (GLint)rb->format; break;
    case GL_RENDERBUFFER_RED_SIZE:        *params = bits[0]; break;
    case GL_RENDERBUFFER_GREEN_SIZE:      *params = bits[1]; break;
    case GL_RENDERBUFFER_BLUE_SIZE:       *params = bits[2]; break;
    case GL_RENDERBUFFER_ALPHA_SIZE:      *params = bits[3]; break;
    case GL_RENDERBUFFER_DEPTH_SIZE:      *params = bits[4]; break;
    case GL_RENDERBUFFER_STENCIL_SIZE:    *params = bits[5]; break;
    default:                              ogles2_error(ctx, GL_INVALID_ENUM); break;
    }
}

void ogles2_glGetFramebufferAttachmentParameteriv(struct OGLES2Context *ctx, GLenum target,
                                                  GLenum attachment, GLenum pname, GLint *params)
{
    const struct OGLES2Attachment *at;
    int a = attachment_index(attachment);
    if (!ogles2_context_valid(ctx) || !params)
        return;
    if (target != GL_FRAMEBUFFER || a < 0) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!ctx->draw_fb) { /* GLES 2.0 6.1.13: the default framebuffer has no attachments to ask */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    at = &ctx->draw_fb->att[a];
    if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) {
        *params = (GLint)at->type;
        return;
    }
    if (at->type == GL_NONE) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
        *params = at->type == GL_TEXTURE ? (GLint)ogles2_texture_name(at->tex) : (GLint)at->rb->name;
        return;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
        if (at->type == GL_TEXTURE) {
            *params = at->level;
            return;
        }
        break;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
        if (at->type == GL_TEXTURE) {
            *params = ogles2_texture_target(at->tex) == GL_TEXTURE_CUBE_MAP
                          ? (GLint)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + at->face) : 0;
            return;
        }
        break;
    }
    ogles2_error(ctx, GL_INVALID_ENUM);
}

/* glGet* (state.c asks): the bindings, the limit, the draw framebuffer's bits. */
BOOL ogles2_fbo_query(struct OGLES2Context *ctx, GLenum pname, GLint *v)
{
    struct OGLES2Framebuffer *fb = ctx->draw_fb;
    GLint r = 8, g = 8, b = 8, a = 8, d, s;
    switch (pname) {
    case GL_FRAMEBUFFER_BINDING:  *v = fb ? (GLint)fb->name : 0; return TRUE;
    case GL_RENDERBUFFER_BINDING: *v = ctx->fbo && ctx->fbo->rb_bound ? (GLint)ctx->fbo->rb_bound->name : 0; return TRUE;
    case GL_MAX_RENDERBUFFER_SIZE: *v = OGLES2_MAX_VIEWPORT; return TRUE;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS:
        if (fb) {
            const struct OGLES2Attachment *c = &fb->att[ATT_COLOR];
            r = g = b = a = 0;
            if (c->type == GL_RENDERBUFFER)
                rb_bits(c->rb->format, &r, &g, &b, &a, &d, &s);
            else if (c->type == GL_TEXTURE)
                tex_bits(c, &r, &g, &b, &a);
        }
        *v = pname == GL_RED_BITS ? r : pname == GL_GREEN_BITS ? g : pname == GL_BLUE_BITS ? b : a;
        return TRUE;
    case GL_DEPTH_BITS:   ogles2_fb_update(ctx); *v = ctx->fb_depth_bits; return TRUE;
    case GL_STENCIL_BITS: ogles2_fb_update(ctx); *v = ctx->fb_stencil_bits; return TRUE;
    default:
        return FALSE;
    }
}

/* aglDestroyContext, after the last job: the framebuffers; the
 * renderbuffers with the share group's last member. */
void ogles2_fbo_free(struct OGLES2Context *ctx)
{
    struct OGLES2FboState *fs = ctx->fbo;
    struct OGLES2Share *sh = ctx->share;
    GLuint i;
    if (sh && sh->members == 1) {
        for (i = 1; i < sh->nrbs; i++)
            if (sh->rbs[i]) {
                rb_free_storage(ctx, sh->rbs[i]);
                FreeVec(sh->rbs[i]);
            }
        if (sh->rbs)
            FreeVec(sh->rbs);
        sh->rbs = 0;
        sh->nrbs = 0;
    }
    if (!fs)
        return;
    for (i = 1; i < fs->nfbs; i++)
        if (fs->fbs[i])
            fb_free(fs->fbs[i]);
    if (fs->fbs)
        FreeVec(fs->fbs);
    FreeVec(fs);
    ctx->fbo = 0;
    ctx->draw_fb = ctx->job_fb = 0;
}
