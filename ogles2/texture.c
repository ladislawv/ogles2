/*
 * Textures v1 (plan section 7.6): 2D and cube map texture objects, glTexImage2D and
 * glTexSubImage2D, parameters, glGenerateMipmap, glPixelStorei, and the
 * texture and sampler state the draw path hands to the TMU_CONFIG recipes.
 *
 * Each level keeps an RGBA8 copy in RAM, converted with GL's rules
 * (GL_LUMINANCE -> (L,L,L,1), GL_ALPHA -> (0,0,0,A), the 16-bit formats
 * expanded), for glGenerateMipmap and for re-uploads when the mip chain
 * grows. On the GPU the texture is stored in the hardware format level 0's
 * format and type map to (plan 7.6, Mesa's v3dx_format_table.c: RGB565,
 * RGBA4, RGB5_A1, R8 and RG8 with swizzles, else RGBA8), packed from those
 * copies at upload, with PiStorm3D's layout code (v3dcore's v3d_texture.c:
 * Mesa's level layout, UIF/UBLINEAR/LINEARTILE tiling).
 *
 * A texture changed after a draw of the open job used it is renamed (plan
 * 7.7 hazard 3): the job keeps the old GPU copy, the texture gets a new one.
 * Cube maps: six faces, each with its own levels; on the GPU six copies of
 * the 2D layout one after another (Mesa's cube_map_stride), sampled with
 * an image depth of 6 and that stride.
 *
 * Render to texture (plan 7.7, fbo.c): a level a framebuffer object's job
 * drew into is newer on the GPU than its RAM copy (OGLES2Face.rendered);
 * the copy is read back (sync) before anything works from it.
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_clbuf.h"
#include "v3d_hw.h"
#include "v3d_texture.h"
#include "etc1.h"

#define MAX_LEVELS 13 /* 4096 = 2^12 */
#define MAX_SIZE   4096

/* The images of one face (a 2D texture has one). */
struct OGLES2Face
{
    UWORD w[MAX_LEVELS], h[MAX_LEVELS];
    ULONG defined;                  /* levels given (bit per level) */
    GLenum format[MAX_LEVELS];      /* each level's internal format (glTexSubImage2D must match it) */
    GLenum type[MAX_LEVELS];        /* and the type it was given in (picks the stored format) */
    UBYTE *shadow[MAX_LEVELS];      /* RGBA8 copy of each level */
    ULONG rendered;                 /* levels the GPU drew into since: the copy is stale */
};

struct OGLES2Texture
{
    GLuint name;
    GLenum target;                  /* 0 until first bound */
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    struct OGLES2Face f[6];         /* 2D: f[0]; a cube map: +X, -X, +Y, -Y, +Z, -Z */
    V3DTexture *gpu;                /* NULL until level 0 exists (a cube map: of all faces) */
    UBYTE hw_type, cpp, swizzle[4]; /* the stored format (storage_of), set with gpu */
    ULONG face_stride;              /* a cube map: bytes from one face's levels to the next */
    ULONG used_serial;              /* the job that last drew with it */
};

/* ---- the context's texture state ---- */

struct OGLES2TexState
{
    struct OGLES2Texture **names;   /* by name */
    GLuint nnames;
    struct OGLES2Texture default2d, defaultcube; /* name 0 */
    struct OGLES2Texture *unit2d[OGLES2_MAX_TEXTURE_UNITS], *unitcube[OGLES2_MAX_TEXTURE_UNITS];
    struct OGLES2Texture fallback;  /* 1x1 (0,0,0,1): what an incomplete texture samples */
    UBYTE active;
    BOOL warned_flush;
};

static void sync(struct OGLES2Texture *t);

static void init_object(struct OGLES2Texture *t, GLuint name, GLenum target)
{
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->target = target;
    t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    t->mag_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_REPEAT;
}

static struct OGLES2TexState *ts_get(struct OGLES2Context *ctx)
{
    struct OGLES2TexState *ts = ctx->tex;
    int i;
    if (ts)
        return ts;
    if (!(ts = AllocVec(sizeof(*ts), MEMF_ANY | MEMF_CLEAR)))
        return 0;
    init_object(&ts->default2d, 0, GL_TEXTURE_2D);
    init_object(&ts->defaultcube, 0, GL_TEXTURE_CUBE_MAP);
    for (i = 0; i < OGLES2_MAX_TEXTURE_UNITS; i++) {
        ts->unit2d[i] = &ts->default2d;
        ts->unitcube[i] = &ts->defaultcube;
    }
    ctx->tex = ts;
    return ts;
}

static void free_gpu(struct OGLES2Texture *t)
{
    if (t->gpu) {
        if (t->gpu->texture_mem.hostptr)
            v3d_texture_free(ogles2_device(), t->gpu);
        FreeVec(t->gpu);
        t->gpu = 0;
    }
}

static void free_levels(struct OGLES2Texture *t)
{
    int k, l;
    for (k = 0; k < 6; k++) {
        for (l = 0; l < MAX_LEVELS; l++) {
            if (t->f[k].shadow[l])
                FreeVec(t->f[k].shadow[l]);
            t->f[k].shadow[l] = 0;
        }
        t->f[k].defined = 0;
    }
    free_gpu(t);
}

void ogles2_textures_free(struct OGLES2Context *ctx)
{
    struct OGLES2TexState *ts = ctx->tex;
    GLuint i;
    if (!ts)
        return;
    for (i = 1; i < ts->nnames; i++)
        if (ts->names[i]) {
            free_levels(ts->names[i]);
            FreeVec(ts->names[i]);
        }
    if (ts->names)
        FreeVec(ts->names);
    free_levels(&ts->default2d);
    free_levels(&ts->defaultcube);
    free_levels(&ts->fallback);
    FreeVec(ts);
    ctx->tex = 0;
}

/* A texture is about to change while a draw of the open job uses it (plan
 * 7.7 hazard 3): that draw must still read the old contents. Renaming, as
 * Mesa does: the GPU copy goes to the job (freed when the frame has
 * rendered, v3d_frame_defer_free), and the texture gets a new one, rebuilt
 * from the RAM copies at its next upload or draw. Only when the job's
 * retire list is nearly full is the job rendered first instead, which
 * costs a wait and the frame's depth and stencil (plan 7.2). */
static void before_change(struct OGLES2Context *ctx, struct OGLES2TexState *ts,
                          struct OGLES2Texture *t)
{
    V3DContext *v = ctx->v3d;
    /* The open job draws into it (a framebuffer object's colour image):
     * render that job first. Then the RAM copies are made current. */
    ogles2_fbo_texture_changing(ctx, t);
    sync(t);
    if (!v || !ogles2_job_has_draws(ctx) || t->used_serial != ctx->job_serial)
        return;
    if (t->gpu && v->frame.retire_count < V3D_RETIRE_MAX - 8) {
        if (t->gpu->texture_mem.hostptr)
            v3d_frame_defer_free(&v->frame, &t->gpu->texture_mem);
        FreeVec(t->gpu);
        t->gpu = 0;
        t->used_serial = 0;
        if (!ts->warned_flush) {
            ts->warned_flush = TRUE;
            ogles2_log("texture %ld changed after a draw used it in the same frame: renamed "
                       "(logged once)", (ULONG)t->name);
        }
        return;
    }
    ogles2_log("texture %ld changed after a draw used it: the frame is rendered first "
               "(too many renamed blocks in this frame)", (ULONG)t->name);
    ogles2_job_flush(ctx, FALSE);
}

/* ---- names and binding ---- */

/* Makes `name` a valid index of the name table. */
static BOOL table_room(struct OGLES2TexState *ts, GLuint name)
{
    GLuint nn, j;
    struct OGLES2Texture **tab;
    if (name < ts->nnames)
        return TRUE;
    nn = ts->nnames ? ts->nnames : 32;
    while (nn <= name)
        nn *= 2;
    if (!(tab = AllocVec(nn * sizeof(*tab), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    for (j = 0; j < ts->nnames; j++)
        tab[j] = ts->names[j];
    if (ts->names)
        FreeVec(ts->names);
    ts->names = tab;
    ts->nnames = nn;
    return TRUE;
}

/* A new texture object named `name` (the slot must be free). */
static struct OGLES2Texture *create(struct OGLES2TexState *ts, GLuint name)
{
    struct OGLES2Texture *t;
    if (!table_room(ts, name) || !(t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR)))
        return 0;
    init_object(t, name, 0);
    ts->names[name] = t;
    return t;
}

void ogles2_glGenTextures(struct OGLES2Context *ctx, GLsizei n, GLuint *textures)
{
    struct OGLES2TexState *ts;
    GLsizei k;
    GLuint i = 1;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(ts = ts_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < n; k++) {
        while (i < ts->nnames && ts->names[i])
            i++;
        if (!create(ts, i)) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        if (textures)
            textures[k] = i;
    }
}

static struct OGLES2Texture *by_name(struct OGLES2TexState *ts, GLuint name)
{
    return name && name < ts->nnames ? ts->names[name] : 0;
}

void ogles2_glDeleteTextures(struct OGLES2Context *ctx, GLsizei n, const GLuint *textures)
{
    struct OGLES2TexState *ts;
    GLsizei k;
    int u;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(ts = ts_get(ctx)) || !textures)
        return;
    for (k = 0; k < n; k++) {
        struct OGLES2Texture *t = by_name(ts, textures[k]);
        if (!t)
            continue;
        for (u = 0; u < 6; u++)
            t->f[u].rendered = 0; /* going: nothing to read back */
        ogles2_fbo_texture_deleted(ctx, t);
        before_change(ctx, ts, t); /* its memory goes: a queued draw must not read it */
        for (u = 0; u < OGLES2_MAX_TEXTURE_UNITS; u++) { /* deleting unbinds (GLES 2.0 3.7.13) */
            if (ts->unit2d[u] == t)
                ts->unit2d[u] = &ts->default2d;
            if (ts->unitcube[u] == t)
                ts->unitcube[u] = &ts->defaultcube;
        }
        ts->names[t->name] = 0;
        free_levels(t);
        FreeVec(t);
    }
}

GLboolean ogles2_glIsTexture(struct OGLES2Context *ctx, GLuint texture)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return GL_FALSE;
    t = by_name(ts, texture);
    return t && t->target ? GL_TRUE : GL_FALSE;
}

void ogles2_glActiveTexture(struct OGLES2Context *ctx, GLenum texture)
{
    struct OGLES2TexState *ts;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + OGLES2_MAX_TEXTURE_UNITS) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ts->active = (UBYTE)(texture - GL_TEXTURE0);
}

void ogles2_glBindTexture(struct OGLES2Context *ctx, GLenum target, GLuint texture)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!texture) {
        t = target == GL_TEXTURE_2D ? &ts->default2d : &ts->defaultcube;
    } else {
        t = by_name(ts, texture);
        /* GLES2 lets a program bind a name glGenTextures did not return. */
        if (!t && !(t = create(ts, texture))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        if (t->target && t->target != target) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        t->target = target;
    }
    if (target == GL_TEXTURE_2D)
        ts->unit2d[ts->active] = t;
    else
        ts->unitcube[ts->active] = t;
}

/* ---- parameters ---- */

static struct OGLES2Texture *bound(struct OGLES2Context *ctx, struct OGLES2TexState *ts, GLenum target)
{
    if (target == GL_TEXTURE_2D)
        return ts->unit2d[ts->active];
    if (target == GL_TEXTURE_CUBE_MAP)
        return ts->unitcube[ts->active];
    ogles2_error(ctx, GL_INVALID_ENUM);
    return 0;
}

static void tex_param(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLint v)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    GLenum e = (GLenum)v;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)) || !(t = bound(ctx, ts, target)))
        return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        if (e != GL_NEAREST && e != GL_LINEAR && e != GL_NEAREST_MIPMAP_NEAREST &&
            e != GL_LINEAR_MIPMAP_NEAREST && e != GL_NEAREST_MIPMAP_LINEAR && e != GL_LINEAR_MIPMAP_LINEAR)
            break;
        t->min_filter = e;
        return;
    case GL_TEXTURE_MAG_FILTER:
        if (e != GL_NEAREST && e != GL_LINEAR)
            break;
        t->mag_filter = e;
        return;
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        if (e != GL_REPEAT && e != GL_CLAMP_TO_EDGE && e != GL_MIRRORED_REPEAT)
            break;
        if (pname == GL_TEXTURE_WRAP_S)
            t->wrap_s = e;
        else
            t->wrap_t = e;
        return;
    default:
        break;
    }
    ogles2_error(ctx, GL_INVALID_ENUM);
}

void ogles2_glTexParameteri(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLint param)
{
    tex_param(ctx, target, pname, param);
}

void ogles2_glTexParameterf(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLfloat param)
{
    tex_param(ctx, target, pname, (GLint)param);
}

void ogles2_glTexParameteriv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                             const GLint *params)
{
    if (params)
        tex_param(ctx, target, pname, params[0]);
}

void ogles2_glTexParameterfv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                             const GLfloat *params)
{
    if (params)
        tex_param(ctx, target, pname, (GLint)params[0]);
}

void ogles2_glGetTexParameteriv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                GLint *params)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    if (!ogles2_context_valid(ctx) || !params || !(ts = ts_get(ctx)) || !(t = bound(ctx, ts, target)))
        return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: *params = (GLint)t->min_filter; break;
    case GL_TEXTURE_MAG_FILTER: *params = (GLint)t->mag_filter; break;
    case GL_TEXTURE_WRAP_S:     *params = (GLint)t->wrap_s; break;
    case GL_TEXTURE_WRAP_T:     *params = (GLint)t->wrap_t; break;
    default:                    ogles2_error(ctx, GL_INVALID_ENUM); break;
    }
}

void ogles2_glGetTexParameterfv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                GLfloat *params)
{
    GLint v = 0;
    if (!params)
        return;
    ogles2_glGetTexParameteriv(ctx, target, pname, &v);
    *params = (GLfloat)v;
}

void ogles2_glPixelStorei(struct OGLES2Context *ctx, GLenum pname, GLint param)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (param != 1 && param != 2 && param != 4 && param != 8) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (pname == GL_UNPACK_ALIGNMENT)
        ctx->unpack_alignment = (UBYTE)param;
    else if (pname == GL_PACK_ALIGNMENT)
        ctx->pack_alignment = (UBYTE)param;
    else
        ogles2_error(ctx, GL_INVALID_ENUM);
}

/* glGet* for the texture state (state.c asks). */
BOOL ogles2_texture_query(struct OGLES2Context *ctx, GLenum pname, GLint *v)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    switch (pname) {
    case GL_ACTIVE_TEXTURE:       *v = (GLint)(GL_TEXTURE0 + (ts ? ts->active : 0)); return TRUE;
    case GL_TEXTURE_BINDING_2D:   *v = ts ? (GLint)ts->unit2d[ts->active]->name : 0; return TRUE;
    case GL_TEXTURE_BINDING_CUBE_MAP: *v = ts ? (GLint)ts->unitcube[ts->active]->name : 0; return TRUE;
    case GL_UNPACK_ALIGNMENT:     *v = ctx->unpack_alignment; return TRUE;
    case GL_PACK_ALIGNMENT:       *v = ctx->pack_alignment; return TRUE;
    case GL_MAX_TEXTURE_SIZE:
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: *v = MAX_SIZE; return TRUE;
    case GL_MAX_TEXTURE_IMAGE_UNITS:
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *v = OGLES2_MAX_TEXTURE_UNITS; return TRUE;
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: *v = 0; return TRUE;
    default:                      return FALSE;
    }
}

/* ---- images ---- */

/* Bytes per pixel of a valid format/type pair; 0 and the error to set if not. */
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1 /* EXT_texture_format_BGRA8888 */
#endif

static int pixel_size(GLenum format, GLenum type, GLenum *err)
{
    *err = GL_NO_ERROR;
    switch (type) {
    case GL_UNSIGNED_BYTE:
        switch (format) {
        case GL_RGBA:
        case GL_BGRA_EXT:        return 4;
        case GL_RGB:             return 3;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_LUMINANCE:
        case GL_ALPHA:           return 1;
        default:                 *err = GL_INVALID_ENUM; return 0;
        }
    case GL_UNSIGNED_SHORT_5_6_5:
        if (format == GL_RGB) return 2;
        *err = format == GL_RGBA || format == GL_LUMINANCE || format == GL_ALPHA ||
               format == GL_LUMINANCE_ALPHA ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        if (format == GL_RGBA) return 2;
        *err = format == GL_RGB || format == GL_LUMINANCE || format == GL_ALPHA ||
               format == GL_LUMINANCE_ALPHA ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    default:
        *err = GL_INVALID_ENUM;
        return 0;
    }
}

static UBYTE x5(UWORD v) { return (UBYTE)((v << 3) | (v >> 2)); }
static UBYTE x6(UWORD v) { return (UBYTE)((v << 2) | (v >> 4)); }
static UBYTE x4(UWORD v) { return (UBYTE)((v << 4) | v); }

/* w x h pixels of the application's (format, type) at src, rows aligned to
 * the unpack alignment, into RGBA8 at dst (dst_stride bytes per row). */
static void convert(UBYTE *dst, ULONG dst_stride, const UBYTE *src, GLenum format, GLenum type,
                    int w, int h, int align)
{
    GLenum err;
    int bpp = pixel_size(format, type, &err), x, y;
    ULONG src_stride = ((ULONG)(w * bpp) + align - 1) & ~(ULONG)(align - 1);
    for (y = 0; y < h; y++, src += src_stride, dst += dst_stride) {
        const UBYTE *s = src;
        UBYTE *d = dst;
        for (x = 0; x < w; x++, d += 4) {
            UWORD v;
            switch (type) {
            case GL_UNSIGNED_SHORT_5_6_5:
                v = (UWORD)((s[0] << 8) | s[1]); /* big-endian: the 68k's own order */
                s += 2;
                d[0] = x5((UWORD)(v >> 11)); d[1] = x6((UWORD)((v >> 5) & 63)); d[2] = x5((UWORD)(v & 31)); d[3] = 255;
                continue;
            case GL_UNSIGNED_SHORT_4_4_4_4:
                v = (UWORD)((s[0] << 8) | s[1]);
                s += 2;
                d[0] = x4((UWORD)(v >> 12)); d[1] = x4((UWORD)((v >> 8) & 15));
                d[2] = x4((UWORD)((v >> 4) & 15)); d[3] = x4((UWORD)(v & 15));
                continue;
            case GL_UNSIGNED_SHORT_5_5_5_1:
                v = (UWORD)((s[0] << 8) | s[1]);
                s += 2;
                d[0] = x5((UWORD)(v >> 11)); d[1] = x5((UWORD)((v >> 6) & 31));
                d[2] = x5((UWORD)((v >> 1) & 31)); d[3] = (v & 1) ? 255 : 0;
                continue;
            }
            switch (format) {
            case GL_RGBA:            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; s += 4; break;
            case GL_BGRA_EXT:        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; s += 4; break;
            case GL_RGB:             d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; s += 3; break;
            case GL_LUMINANCE_ALPHA: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; s += 2; break;
            case GL_LUMINANCE:       d[0] = d[1] = d[2] = s[0]; d[3] = 255; s += 1; break;
            default: /* GL_ALPHA */  d[0] = d[1] = d[2] = 0; d[3] = s[0]; s += 1; break;
            }
        }
    }
}

static int level_count(int w, int h)
{
    int n = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        n++;
    }
    return n;
}

/* Level `l` of a face has the size the mip chain of its level 0 gives it. */
static BOOL consistent(const struct OGLES2Face *f, int l)
{
    int w = f->w[0] >> l, h = f->h[0] >> l;
    if (!(f->defined & (1UL << l)) || !(f->defined & 1))
        return FALSE;
    return f->w[l] == (w ? w : 1) && f->h[l] == (h ? h : 1);
}

static int faces(const struct OGLES2Texture *t)
{
    return t->target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
}

/* Level 0 exists; a cube map's on all six faces, square and of one size
 * (GLES 2.0 3.7.10, "cube complete"). */
static BOOL base_ok(const struct OGLES2Texture *t)
{
    const struct OGLES2Face *f0 = &t->f[0];
    int k;
    if (!(f0->defined & 1) || !f0->w[0] || !f0->h[0])
        return FALSE;
    for (k = 1; k < faces(t); k++)
        if (!(t->f[k].defined & 1) || t->f[k].w[0] != f0->w[0] || t->f[k].h[0] != f0->h[0])
            return FALSE;
    return faces(t) == 1 || f0->w[0] == f0->h[0];
}

static BOOL mipmapped(GLenum min_filter)
{
    return min_filter != GL_NEAREST && min_filter != GL_LINEAR;
}

/* GLES 2.0 3.7.10: level 0 (a cube map's cube complete), and with a
 * mipmapping filter every level down to 1x1 consistent, on every face. */
static BOOL complete(const struct OGLES2Texture *t, int *levels)
{
    int n, l, k;
    if (!t->gpu || !base_ok(t))
        return FALSE;
    n = mipmapped(t->min_filter) ? level_count(t->f[0].w[0], t->f[0].h[0]) : 1;
    for (k = 0; k < faces(t); k++)
        for (l = 1; l < n; l++)
            if (!consistent(&t->f[k], l))
                return FALSE;
    *levels = n;
    return TRUE;
}

/* A face's view of the GPU texture: face 0 is the texture itself; face k
 * the same layout `k` face strides further on (the upload functions only
 * write through texture_mem). */
static V3DTexture *face_gpu(struct OGLES2Texture *t, int k, V3DTexture *tmp)
{
    if (!k)
        return t->gpu;
    *tmp = *t->gpu;
    tmp->texture_mem.hostptr =
        (void *)(V3D_ALIGN_UP((v3d_uintptr)t->gpu->texture_mem.hostptr, 256) + (ULONG)k * t->face_stride);
    tmp->texture_mem.size = t->face_stride;
    return tmp;
}

/* The hardware format for level 0's format and type (Mesa's
 * v3dx_format_table.c, the formats GLES2's state tracker picks). */
static void storage_of(struct OGLES2Texture *t)
{
    GLenum f = t->f[0].format[0], ty = t->f[0].type[0];
    UBYTE R = v3d_SWIZZLE_RED, G = v3d_SWIZZLE_GREEN, B = v3d_SWIZZLE_BLUE, A = v3d_SWIZZLE_ALPHA;
    UBYTE O = v3d_SWIZZLE_ONE, Z = v3d_SWIZZLE_ZERO;
    UBYTE type = V3D_TEXTURE_DATA_FORMAT_RGBA8, cpp = 4, sw[4];
    sw[0] = R; sw[1] = G; sw[2] = B; sw[3] = A;
    if (f == GL_RGB)
        sw[3] = O; /* rendered into, the stored alpha is the shader's */
    if (f == GL_RGB && ty == GL_UNSIGNED_SHORT_5_6_5) {
        type = V3D_TEXTURE_DATA_FORMAT_RGB565; cpp = 2; sw[3] = O;
    } else if (f == GL_RGBA && ty == GL_UNSIGNED_SHORT_4_4_4_4) {
        type = V3D_TEXTURE_DATA_FORMAT_RGBA4; cpp = 2;
    } else if (f == GL_RGBA && ty == GL_UNSIGNED_SHORT_5_5_5_1) {
        type = V3D_TEXTURE_DATA_FORMAT_RGB5A1; cpp = 2;
    } else if (f == GL_LUMINANCE) {
        type = V3D_TEXTURE_DATA_FORMAT_R8; cpp = 1; sw[0] = sw[1] = sw[2] = R; sw[3] = O;
    } else if (f == GL_ALPHA) {
        type = V3D_TEXTURE_DATA_FORMAT_R8; cpp = 1; sw[0] = sw[1] = sw[2] = Z; sw[3] = R;
    } else if (f == GL_LUMINANCE_ALPHA) {
        type = V3D_TEXTURE_DATA_FORMAT_RG8; cpp = 2; sw[0] = sw[1] = sw[2] = R; sw[3] = G;
    }
    t->hw_type = type;
    t->cpp = cpp;
    memcpy(t->swizzle, sw, 4);
}

/* n RGBA8 texels into the stored format, in the GPU's byte order (16-bit
 * texels little-endian, the GL packing: red in the top bits). */
static void pack(const struct OGLES2Texture *t, const UBYTE *s, UBYTE *d, ULONG n)
{
    ULONG i;
    UWORD v;
    for (i = 0; i < n; i++, s += 4) {
        switch (t->hw_type) {
        case V3D_TEXTURE_DATA_FORMAT_RGB565:
            v = (UWORD)(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3));
            *d++ = (UBYTE)v; *d++ = (UBYTE)(v >> 8);
            break;
        case V3D_TEXTURE_DATA_FORMAT_RGBA4:
            v = (UWORD)(((s[0] >> 4) << 12) | ((s[1] >> 4) << 8) | ((s[2] >> 4) << 4) | (s[3] >> 4));
            *d++ = (UBYTE)v; *d++ = (UBYTE)(v >> 8);
            break;
        case V3D_TEXTURE_DATA_FORMAT_RGB5A1:
            v = (UWORD)(((s[0] >> 3) << 11) | ((s[1] >> 3) << 6) | ((s[2] >> 3) << 1) | (s[3] >> 7));
            *d++ = (UBYTE)v; *d++ = (UBYTE)(v >> 8);
            break;
        case V3D_TEXTURE_DATA_FORMAT_R8: /* luminance (red of the copy) or alpha */
            *d++ = t->swizzle[3] == v3d_SWIZZLE_RED ? s[3] : s[0];
            break;
        case V3D_TEXTURE_DATA_FORMAT_RG8: /* luminance, alpha */
            *d++ = s[0]; *d++ = s[3];
            break;
        default:
            *d++ = s[0]; *d++ = s[1]; *d++ = s[2]; *d++ = s[3];
            break;
        }
    }
}

/* The inverse of pack: n texels of the stored format into RGBA8. */
static void unpack(const struct OGLES2Texture *t, const UBYTE *s, UBYTE *d, ULONG n)
{
    ULONG i;
    UWORD v;
    for (i = 0; i < n; i++, d += 4) {
        switch (t->hw_type) {
        case V3D_TEXTURE_DATA_FORMAT_RGB565:
            v = (UWORD)(s[0] | (s[1] << 8)); s += 2;
            d[0] = (UBYTE)(((v >> 11) & 31) * 255 / 31);
            d[1] = (UBYTE)(((v >> 5) & 63) * 255 / 63);
            d[2] = (UBYTE)((v & 31) * 255 / 31);
            d[3] = 255;
            break;
        case V3D_TEXTURE_DATA_FORMAT_RGBA4:
            v = (UWORD)(s[0] | (s[1] << 8)); s += 2;
            d[0] = (UBYTE)(((v >> 12) & 15) * 17);
            d[1] = (UBYTE)(((v >> 8) & 15) * 17);
            d[2] = (UBYTE)(((v >> 4) & 15) * 17);
            d[3] = (UBYTE)((v & 15) * 17);
            break;
        case V3D_TEXTURE_DATA_FORMAT_RGB5A1:
            v = (UWORD)(s[0] | (s[1] << 8)); s += 2;
            d[0] = (UBYTE)(((v >> 11) & 31) * 255 / 31);
            d[1] = (UBYTE)(((v >> 6) & 31) * 255 / 31);
            d[2] = (UBYTE)(((v >> 1) & 31) * 255 / 31);
            d[3] = (v & 1) ? 255 : 0;
            break;
        case V3D_TEXTURE_DATA_FORMAT_R8:
            d[0] = d[1] = d[2] = t->swizzle[3] == v3d_SWIZZLE_RED ? 0 : s[0];
            d[3] = t->swizzle[3] == v3d_SWIZZLE_RED ? s[0] : 255;
            s++;
            break;
        case V3D_TEXTURE_DATA_FORMAT_RG8:
            d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; s += 2;
            break;
        default:
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            d[3] = t->swizzle[3] == v3d_SWIZZLE_ONE ? 255 : s[3];
            s += 4;
            break;
        }
    }
}

/* A box of face k, level l as RGBA8 rows from the GPU's copy. */
static BOOL load_box(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h, UBYTE *rgba,
                     ULONG stride)
{
    V3DTexture tmp;
    ULONG row = (ULONG)w * t->cpp;
    UBYTE *packed;
    int j;
    if (!t->gpu || l >= t->gpu->num_levels || w <= 0 || h <= 0)
        return FALSE;
    if (!(packed = AllocVec(row * h + 4, MEMF_ANY)))
        return FALSE;
    v3d_texture_load(ogles2_device(), face_gpu(t, k, &tmp), (v3d_u8)l, packed, row, x, y, w, h);
    for (j = 0; j < h; j++)
        unpack(t, packed + (ULONG)j * row, rgba + (ULONG)j * stride, (ULONG)w);
    FreeVec(packed);
    return TRUE;
}

/* The RAM copies of every level the GPU drew into, read back. */
static void sync(struct OGLES2Texture *t)
{
    int k, l;
    for (k = 0; k < 6; k++) {
        struct OGLES2Face *f = &t->f[k];
        if (!f->rendered)
            continue;
        for (l = 0; l < MAX_LEVELS; l++)
            if ((f->rendered & (1UL << l)) && f->shadow[l])
                load_box(t, k, l, 0, 0, f->w[l], f->h[l], f->shadow[l], (ULONG)f->w[l] * 4);
        f->rendered = 0;
    }
}

/* A box of level l of face k, from the RGBA8 copy `src` (`w` texels a row). */
static void store_box(struct OGLES2Texture *t, int k, int l, const UBYTE *src, int x, int y, int w,
                      int h)
{
    V3DTexture tmp;
    ULONG row = (ULONG)w * t->cpp;
    UBYTE *packed = AllocVec(row * h + 4, MEMF_ANY);
    int j;
    if (!packed)
        return;
    for (j = 0; j < h; j++)
        pack(t, src + (ULONG)j * w * 4, packed + (ULONG)j * row, (ULONG)w);
    v3d_texture_store(ogles2_device(), face_gpu(t, k, &tmp), (v3d_u8)l, packed, row, x, y, w, h);
    FreeVec(packed);
}

static void upload_level(struct OGLES2Texture *t, int k, int l)
{
    store_box(t, k, l, t->f[k].shadow[l], 0, 0, t->f[k].w[l], t->f[k].h[l]);
}

/* A cube map's memory: six of the 2D layout v3d_texture_alloc(_mipchain)
 * made, `face_stride` apart (Mesa: the tree's end aligned to 64 bytes; a
 * page here, so each face's level 0 keeps the alignment UIF XOR needs). */
static BOOL cube_memory(struct OGLES2Texture *t)
{
    V3DTexture *g = t->gpu;
    v3d_mem m;
    ULONG tree = g->levels[0].offset + (ULONG)g->levels[0].padded_h * g->levels[0].stride;
    t->face_stride = (tree + 4095) & ~4095UL;
    if (v3d_mem_alloc(ogles2_device(), &m, t->face_stride * 6 + 256) < 0)
        return FALSE;
    v3d_mem_free(ogles2_device(), &g->texture_mem);
    g->texture_mem = m;
    return TRUE;
}

/* (Re)creates the GPU texture for the levels given and uploads them. */
static BOOL upload_all(struct OGLES2Texture *t)
{
    V3DDevice *dev = ogles2_device();
    const struct OGLES2Face *f0 = &t->f[0];
    int l, k, n = 1;
    sync(t);
    free_gpu(t);
    if (!base_ok(t))
        return TRUE; /* nothing to sample yet (a cube map: until all faces agree) */
    for (l = 1; l < MAX_LEVELS; l++)
        if (consistent(f0, l))
            n = l + 1;
    if (!(t->gpu = AllocVec(sizeof(*t->gpu), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    storage_of(t);
    if (v3d_texture_alloc_cpp(dev, t->gpu, f0->w[0], f0->h[0], t->cpp) < 0 ||
        (n > 1 && v3d_texture_alloc_mipchain(dev, t->gpu, (v3d_u8)n) < 0)) {
        free_gpu(t);
        return FALSE;
    }
    /* Growing the chain parks the level-0 block it replaces on v3dcore's
     * park list for PiStorm3D's mid-frame renaming. Here nothing can use it
     * (the job that last drew with this texture was rendered before the
     * change, before_change), and ogles2 is the list's only user: free it. */
    if (n > 1)
        v3d_texture_drain_parked_all(dev);
    if (faces(t) > 1 && !cube_memory(t)) {
        free_gpu(t);
        return FALSE;
    }
    for (k = 0; k < faces(t); k++)
        for (l = 0; l < n; l++)
            if (l == 0 || consistent(&t->f[k], l))
                upload_level(t, k, l);
    return TRUE;
}

static BOOL face_target(GLenum target)
{
    return target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
}

/* ---- for framebuffer objects (fbo.c) ---- */

struct OGLES2Texture *ogles2_texture_get(struct OGLES2Context *ctx, GLuint name)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    return ts ? by_name(ts, name) : 0;
}

GLuint ogles2_texture_name(const struct OGLES2Texture *t)
{
    return t->name;
}

GLenum ogles2_texture_target(const struct OGLES2Texture *t)
{
    return t->target;
}

BOOL ogles2_texture_image(const struct OGLES2Texture *t, int k, int l, UWORD *w, UWORD *h,
                          GLenum *format, GLenum *type)
{
    const struct OGLES2Face *f = &t->f[k];
    if (k < 0 || k > 5 || l < 0 || l >= MAX_LEVELS || !(f->defined & (1UL << l)))
        return FALSE;
    *w = f->w[l];
    *h = f->h[l];
    *format = f->format[l];
    *type = f->type[l];
    return TRUE;
}

/* GLES2's colour-renderable texture images (plan 7.7): RGB and RGBA in the
 * formats the TLB stores; the GPU copy is in level 0's format, so the level
 * must have level 0's format and type. */
BOOL ogles2_texture_renderable(const struct OGLES2Texture *t, int k, int l)
{
    const struct OGLES2Face *f = &t->f[k], *f0 = &t->f[0];
    GLenum fm, ty;
    if (!(f->defined & (1UL << l)) || !(f0->defined & 1))
        return FALSE;
    fm = f->format[l];
    ty = f->type[l];
    if (fm != f0->format[0] || ty != f0->type[0])
        return FALSE;
    return (ty == GL_UNSIGNED_BYTE && (fm == GL_RGB || fm == GL_RGBA)) ||
           (ty == GL_UNSIGNED_SHORT_5_6_5 && fm == GL_RGB) ||
           ((ty == GL_UNSIGNED_SHORT_4_4_4_4 || ty == GL_UNSIGNED_SHORT_5_5_5_1) && fm == GL_RGBA);
}

BOOL ogles2_texture_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, int l,
                            struct OGLES2Surface *s)
{
    const V3DTexture *g;
    ULONG cpp;
    if (!ogles2_texture_renderable(t, k, l))
        return FALSE;
    if ((!t->gpu || l >= t->gpu->num_levels) && !upload_all(t))
        return FALSE;
    if (!(g = t->gpu) || l >= g->num_levels)
        return FALSE; /* not in the chain (a level of another size), or a cube map not complete */
    cpp = t->cpp;
    s->addr = V3D_ALIGN_UP((v3d_uintptr)g->texture_mem.hostptr, 256) + (ULONG)k * t->face_stride +
              g->levels[l].offset;
    s->bytes = g->levels[l].stride * g->levels[l].padded_h;
    s->mem_format = g->levels[l].tiling_format;
    s->format = t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGB565 ? V3D_OUTPUT_IMAGE_FORMAT_BGR565
              : t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA4  ? V3D_OUTPUT_IMAGE_FORMAT_ABGR4444
              : t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGB5A1 ? V3D_OUTPUT_IMAGE_FORMAT_ABGR1555
                                                             : V3D_OUTPUT_IMAGE_FORMAT_RGBA8;
    s->force_alpha1 = t->f[k].format[l] == GL_RGB;
    s->ub = s->mem_format == V3D_MEMORY_FORMAT_UIF_NO_XOR || s->mem_format == V3D_MEMORY_FORMAT_UIF_XOR
                ? g->levels[l].padded_h / (2 * v3d_utile_height((int)cpp))
                : 0;
    (void)ctx;
    return TRUE;
}

void ogles2_texture_rendered(struct OGLES2Texture *t, int k, int l)
{
    t->f[k].rendered |= 1UL << l;
}

BOOL ogles2_texture_read(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h,
                         UBYTE *rgba, ULONG stride)
{
    const struct OGLES2Face *f = &t->f[k];
    int j;
    if (load_box(t, k, l, x, y, w, h, rgba, stride))
        return TRUE;
    if (!f->shadow[l]) /* no GPU copy (a state-only context): the RAM copy */
        return FALSE;
    for (j = 0; j < h; j++)
        memcpy(rgba + (ULONG)j * stride, f->shadow[l] + ((ULONG)(y + j) * f->w[l] + x) * 4, (ULONG)w * 4);
    return TRUE;
}

struct OGLES2Texture *ogles2_texture_hidden(struct OGLES2Context *ctx, UWORD w, UWORD h, GLenum format,
                                            GLenum type)
{
    struct OGLES2Texture *t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR);
    if (!t)
        return 0;
    init_object(t, 0, GL_TEXTURE_2D);
    t->min_filter = GL_NEAREST;
    t->f[0].shadow[0] = AllocVec((ULONG)w * h * 4 + 4, MEMF_ANY | MEMF_CLEAR);
    t->f[0].w[0] = w;
    t->f[0].h[0] = h;
    t->f[0].format[0] = format;
    t->f[0].type[0] = type;
    t->f[0].defined = 1;
    if (!t->f[0].shadow[0] || (ctx->v3d && (!upload_all(t) || !t->gpu))) {
        ogles2_texture_hidden_free(ctx, t);
        return 0;
    }
    return t;
}

void ogles2_texture_hidden_free(struct OGLES2Context *ctx, struct OGLES2Texture *t)
{
    (void)ctx;
    if (!t)
        return;
    t->f[0].rendered = 0;
    free_levels(t);
    FreeVec(t);
}

void ogles2_glTexImage2D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
                         const void *pixels)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    struct OGLES2Face *f;
    GLenum err;
    UBYTE *img;
    int k;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!pixel_size(format, type, &err)) {
        ogles2_error(ctx, err);
        return;
    }
    if (level < 0 || level >= MAX_LEVELS || width < 0 || height < 0 || border != 0 ||
        width > (MAX_SIZE >> level) || height > (MAX_SIZE >> level) ||
        (face_target(target) && width != height)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if ((GLenum)internalformat != format) {
        ogles2_error(ctx, internalformat == GL_RGBA || internalformat == GL_RGB ||
                          internalformat == GL_LUMINANCE || internalformat == GL_ALPHA ||
                          internalformat == GL_LUMINANCE_ALPHA ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
        return;
    }
    t = face_target(target) ? ts->unitcube[ts->active] : ts->unit2d[ts->active];
    k = face_target(target) ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    f = &t->f[k];
    before_change(ctx, ts, t);
    img = width && height ? AllocVec((ULONG)width * height * 4, MEMF_ANY | MEMF_CLEAR) : 0;
    if (width && height && !img) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (img && pixels)
        convert(img, (ULONG)width * 4, pixels, format, type, width, height, ctx->unpack_alignment);
    if (f->shadow[level])
        FreeVec(f->shadow[level]);
    f->shadow[level] = img;
    f->w[level] = (UWORD)width;
    f->h[level] = (UWORD)height;
    f->format[level] = format;
    f->type[level] = type;
    f->defined |= 1UL << level;
    if (!ctx->v3d)
        return; /* a state-only context keeps the images only */
    /* No GPU texture yet, a new level 0, or a level the chain has no room
     * for: rebuild (a cube map's faces arrive one by one: rebuilt each time
     * until the six agree, then per level). */
    if (!t->gpu || (level == 0 && (faces(t) == 1 || t->gpu->width != f->w[0])) ||
        level >= t->gpu->num_levels || t->gpu->width != t->f[0].w[0] || t->gpu->height != t->f[0].h[0]) {
        if (!upload_all(t))
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
    } else if (consistent(f, level) && img) {
        upload_level(t, k, level);
    }
}

void ogles2_glTexSubImage2D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint xoffset,
                            GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
                            const void *pixels)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    struct OGLES2Face *f;
    GLenum err;
    UBYTE *sub;
    int y, k;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!pixel_size(format, type, &err)) {
        ogles2_error(ctx, err);
        return;
    }
    t = face_target(target) ? ts->unitcube[ts->active] : ts->unit2d[ts->active];
    k = face_target(target) ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    f = &t->f[k];
    if (level < 0 || level >= MAX_LEVELS || !(f->defined & (1UL << level))) {
        ogles2_error(ctx, level < 0 || level >= MAX_LEVELS ? GL_INVALID_VALUE : GL_INVALID_OPERATION);
        return;
    }
    if (format != f->format[level]) { /* GLES 2.0 3.7.2: the level's own format */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (xoffset < 0 || yoffset < 0 || width < 0 || height < 0 ||
        xoffset + width > f->w[level] || yoffset + height > f->h[level]) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!width || !height || !pixels)
        return;
    before_change(ctx, ts, t);
    if (!(sub = AllocVec((ULONG)width * height * 4, MEMF_ANY))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    convert(sub, (ULONG)width * 4, pixels, format, type, width, height, ctx->unpack_alignment);
    for (y = 0; y < height; y++)
        memcpy(f->shadow[level] + ((ULONG)(yoffset + y) * f->w[level] + xoffset) * 4,
               sub + (ULONG)y * width * 4, (ULONG)width * 4);
    if (t->gpu && level < t->gpu->num_levels && consistent(f, level))
        store_box(t, k, level, sub, xoffset, yoffset, width, height);
    else if (!t->gpu && ctx->v3d && !upload_all(t)) /* renamed: a new copy, whole */
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
    FreeVec(sub);
}

/* ---- copies from the framebuffer (glCopyTexImage2D, glCopyTexSubImage2D) ----
 * The region is read as glReadPixels reads it (the open job rendered first,
 * rows bottom-up, which is a texture's row order too), then given to
 * glTexImage2D/glTexSubImage2D, so targets, levels and errors are theirs.
 * The default framebuffer is RGBA8, from which every GLES2 format may be
 * copied (GLES 2.0 3.8.2, table 3.15). */

static int base_components(GLenum format)
{
    switch (format) {
    case GL_ALPHA: case GL_LUMINANCE: return 1;
    case GL_LUMINANCE_ALPHA:          return 2;
    case GL_RGB:                      return 3;
    case GL_RGBA:                     return 4;
    default:                          return 0;
    }
}

/* width x height pixels from (x, y) as `format` bytes, tightly packed
 * (unpack alignment 1): LUMINANCE takes red, ALPHA alpha, RGB drops alpha
 * (table 3.15). Pixels outside the framebuffer read as 0. */
static UBYTE *read_region(struct OGLES2Context *ctx, GLint x, GLint y, GLsizei width, GLsizei height,
                          GLenum format)
{
    ULONG n = (ULONG)width * height, i;
    int c = base_components(format);
    UBYTE *rgba = AllocVec(n * 4 + 1, MEMF_ANY | MEMF_CLEAR), *out;
    UBYTE pack = ctx->pack_alignment;
    if (!rgba)
        return 0;
    ctx->pack_alignment = 1;
    ogles2_glReadPixels(ctx, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    ctx->pack_alignment = pack;
    out = rgba; /* converted in place: each pixel only shrinks */
    for (i = 0; i < n; i++) {
        const UBYTE *s = rgba + i * 4;
        UBYTE *d = out + i * c;
        UBYTE r = s[0], g = s[1], b = s[2], a = s[3];
        switch (format) {
        case GL_ALPHA:           d[0] = a; break;
        case GL_LUMINANCE:       d[0] = r; break;
        case GL_LUMINANCE_ALPHA: d[0] = r; d[1] = a; break;
        case GL_RGB:             d[0] = r; d[1] = g; d[2] = b; break;
        default:                 d[0] = r; d[1] = g; d[2] = b; d[3] = a; break;
        }
    }
    return out;
}

void ogles2_glCopyTexImage2D(struct OGLES2Context *ctx, GLenum target, GLint level,
                             GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height,
                             GLint border)
{
    UBYTE *img, unpack;
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_TEXTURE_2D && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!base_components(internalformat)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0 || border != 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(img = read_region(ctx, x, y, width, height, internalformat))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    unpack = ctx->unpack_alignment;
    ctx->unpack_alignment = 1;
    ogles2_glTexImage2D(ctx, target, level, (GLint)internalformat, width, height, 0, internalformat,
                        GL_UNSIGNED_BYTE, img);
    ctx->unpack_alignment = unpack;
    FreeVec(img);
}

void ogles2_glCopyTexSubImage2D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint xoffset,
                                GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    UBYTE *img, unpack;
    GLenum fmt;
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_TEXTURE_2D && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!width || !height)
        return;
    /* In the level's own format, so the conversion is table 3.15's (a
     * LUMINANCE texture takes red); glTexSubImage2D reports a level that
     * does not exist. */
    {
        struct OGLES2TexState *ts = ts_get(ctx);
        struct OGLES2Texture *t;
        if (!ts)
            return;
        t = face_target(target) ? ts->unitcube[ts->active] : ts->unit2d[ts->active];
        fmt = level >= 0 && level < MAX_LEVELS ? t->f[face_target(target) ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0].format[level] : 0;
        if (!base_components(fmt))
            fmt = GL_RGBA; /* an undefined level: glTexSubImage2D refuses it */
    }
    if (!(img = read_region(ctx, x, y, width, height, fmt))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    unpack = ctx->unpack_alignment;
    ctx->unpack_alignment = 1;
    ogles2_glTexSubImage2D(ctx, target, level, xoffset, yoffset, width, height, fmt,
                           GL_UNSIGNED_BYTE, img);
    ctx->unpack_alignment = unpack;
    FreeVec(img);
}

/* ---- compressed textures: OES_compressed_ETC1_RGB8_texture ----
 * Decoded into the RGBA8 storage (etc1.c) and stored as glTexImage2D does;
 * the level then keeps the ETC1 format, which no TexSubImage may change. */
#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif

void ogles2_glCompressedTexImage2D(struct OGLES2Context *ctx, GLenum target, GLint level,
                                   GLenum internalformat, GLsizei width, GLsizei height, GLint border,
                                   GLsizei imageSize, const void *data)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    ULONG bw, bh;
    UBYTE *img, unpack;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (internalformat != GL_ETC1_RGB8_OES) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    bw = ((ULONG)width + 3) / 4;
    bh = ((ULONG)height + 3) / 4;
    if (width < 0 || height < 0 || border != 0 || level < 0 || level >= MAX_LEVELS ||
        imageSize != (GLsizei)(bw * bh * 8)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    img = width && height ? AllocVec((ULONG)width * height * 4, MEMF_ANY | MEMF_CLEAR) : 0;
    if (width && height && !img) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (img && data)
        ogles2_etc1_decode((const UBYTE *)data, (ULONG)width, (ULONG)height, img);
    unpack = ctx->unpack_alignment;
    ctx->unpack_alignment = 1;
    ogles2_glTexImage2D(ctx, target, level, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    ctx->unpack_alignment = unpack;
    if (img)
        FreeVec(img);
    /* The level is ETC1: glTexSubImage2D (and ETC1's CompressedTexSubImage2D)
     * may not change it. */
    t = face_target(target) ? ts->unitcube[ts->active] : ts->unit2d[ts->active];
    t->f[face_target(target) ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0].format[level] =
        GL_ETC1_RGB8_OES;
}

void ogles2_glCompressedTexSubImage2D(struct OGLES2Context *ctx, GLenum target, GLint level,
                                      GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                                      GLenum format, GLsizei imageSize, const void *data)
{
    (void)level; (void)xoffset; (void)yoffset; (void)width; (void)height; (void)imageSize; (void)data;
    if (!ogles2_context_valid(ctx))
        return;
    if ((target != GL_TEXTURE_2D && !face_target(target)) || format != GL_ETC1_RGB8_OES) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* OES_compressed_ETC1_RGB8_texture: ETC1 images cannot be updated. */
    ogles2_error(ctx, GL_INVALID_OPERATION);
}

/* Level l+1 from level l: each texel the mean of a 2x2 box (a 1-texel edge
 * reuses its row or column). */
static void box_down(const UBYTE *src, int sw, int sh, UBYTE *dst, int dw, int dh)
{
    int x, y, c;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            int x0 = x * 2, y0 = y * 2;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
            for (c = 0; c < 4; c++) {
                ULONG sum = src[(y0 * sw + x0) * 4 + c] + src[(y0 * sw + x1) * 4 + c] +
                            src[(y1 * sw + x0) * 4 + c] + src[(y1 * sw + x1) * 4 + c];
                dst[(y * dw + x) * 4 + c] = (UBYTE)((sum + 2) / 4);
            }
        }
}

void ogles2_glGenerateMipmap(struct OGLES2Context *ctx, GLenum target)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    int l, n, k;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)) || !(t = bound(ctx, ts, target)))
        return;
    if (!base_ok(t)) {
        /* GLES 2.0 3.7.11: a cube map that is not cube complete. */
        if (target == GL_TEXTURE_CUBE_MAP)
            ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    before_change(ctx, ts, t);
    n = level_count(t->f[0].w[0], t->f[0].h[0]);
    for (k = 0; k < faces(t); k++) {
        struct OGLES2Face *f = &t->f[k];
        for (l = 1; l < n; l++) {
            int w = f->w[l - 1] > 1 ? f->w[l - 1] / 2 : 1, h = f->h[l - 1] > 1 ? f->h[l - 1] / 2 : 1;
            UBYTE *img = AllocVec((ULONG)w * h * 4, MEMF_ANY);
            if (!img) {
                ogles2_error(ctx, GL_OUT_OF_MEMORY);
                return;
            }
            box_down(f->shadow[l - 1], f->w[l - 1], f->h[l - 1], img, w, h);
            if (f->shadow[l])
                FreeVec(f->shadow[l]);
            f->shadow[l] = img;
            f->w[l] = (UWORD)w;
            f->h[l] = (UWORD)h;
            f->format[l] = f->format[l - 1];
            f->type[l] = f->type[l - 1];
            f->defined |= 1UL << l;
        }
    }
    if (ctx->v3d && !upload_all(t))
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
}

/* ---- for the draw path ---- */

/* The fallback texture, 1x1 (0,0,0,1) (GLES 2.0 3.8.2: an incomplete
 * texture samples that). */
static struct OGLES2Texture *fallback(struct OGLES2TexState *ts)
{
    struct OGLES2Texture *t = &ts->fallback;
    if (!t->gpu) {
        init_object(t, 0, GL_TEXTURE_2D);
        t->min_filter = t->mag_filter = GL_NEAREST;
        if (!(t->f[0].shadow[0] = AllocVec(4, MEMF_ANY | MEMF_CLEAR)))
            return 0;
        t->f[0].shadow[0][3] = 255;
        t->f[0].w[0] = t->f[0].h[0] = 1;
        t->f[0].format[0] = GL_RGBA;
        t->f[0].type[0] = GL_UNSIGNED_BYTE;
        t->f[0].defined = 1;
        if (!upload_all(t))
            return 0;
    }
    return t;
}

/* Claims `size` bytes of the state buffer at a 32-byte boundary (the
 * TMU_CONFIG words carry configuration in the address's low bits). */
static void *claim32(V3DContext *v, ULONG size)
{
    v3d_u8 s = v->build_slot;
    v3d_static_buffer *sb = &v->state_buf[s];
    sb->used = (sb->used + 31) & ~31;
    return v3d_cl_claim_fast(ogles2_device(), &v->state_mem[s], sb, size, &v->frame);
}

/* The texture and sampler state for unit `unit`'s texture of `cube`
 * kind, written into the job's state buffer (packing as PiStorm3D's
 * v3d_texture_emit_state; values as Mesa's v3dx_state.c). FALSE: out of
 * memory. */
BOOL ogles2_texture_state(struct OGLES2Context *ctx, int unit, BOOL cube, ULONG *tex_state,
                          ULONG *sampler_state)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    struct OGLES2Texture *t;
    V3DContext *v = ctx->v3d;
    v3d_texture_shader_state *tsr;
    v3d_sampler_state *ss;
    V3DTexture *g;
    char *tiled;
    ULONG *swivel;
    UBYTE *swivel24, temp;
    UWORD *swivel16;
    int levels = 1;
    BOOL mip;

    if (!ts)
        return FALSE;
    t = cube ? ts->unitcube[unit] : ts->unit2d[unit];
    /* A renamed texture gets its new copy at its first draw at the latest. */
    if (!t->gpu && t != &ts->fallback)
        upload_all(t);
    if (!complete(t, &levels)) {
        if (!(t = fallback(ts)))
            return FALSE;
        levels = 1;
    }
    t->used_serial = ctx->job_serial;
    g = t->gpu;
    mip = mipmapped(t->min_filter) && levels > 1;

    if (!(tsr = claim32(v, sizeof(*tsr))))
        return FALSE;
    memset(tsr, 0, sizeof(*tsr));
    tiled = (char *)V3D_ALIGN_UP((v3d_uintptr)g->texture_mem.hostptr, 256);
    tsr->texture_base_pointer_rshift_6 = ((ULONG)tiled + g->levels[0].offset) >> 6;
    tsr->flip_texture_y_axis = FALSE;
    tsr->base_level = 0;
    tsr->max_level = (ULONG)(levels - 1);
    tsr->image_width_lo = g->width & 0x3f;
    tsr->image_width_hi = g->width >> 6;
    tsr->image_height = g->height;
    /* A cube map: six layers, a face stride apart (Mesa's v3dx_state.c). The
     * fallback is 2D: as a cube, every face reads its one texel. */
    tsr->image_depth_lo = faces(t);
    tsr->image_depth_hi = 0;
    tsr->array_stride_64_byte_aligned = faces(t) > 1 ? t->face_stride / 64 : 0;
    tsr->texture_type = t->hw_type;
    tsr->swizzle_r = t->swizzle[0];
    tsr->swizzle_g = t->swizzle[1];
    tsr->swizzle_b = t->swizzle[2];
    tsr->swizzle_a = t->swizzle[3];
    if (g->tiling_format == V3D_MEMORY_FORMAT_UIF_XOR) {
        tsr->level_0_is_strictly_uif = TRUE;
        tsr->level_0_xor_enable = TRUE;
        tsr->level_0_ubpad = g->ub_pad;
        tsr->extended = TRUE;
    }
    swivel = (ULONG *)tsr;
    swivel[0] = LE32(swivel[0]);
    swivel[1] = LE32(swivel[1]);
    swivel[2] = LE32(swivel[2]);
    swivel[3] = LE32(swivel[3]);
    swivel[4] = LE32(swivel[4]);
    *tex_state = (ULONG)tsr;

    if (!(ss = claim32(v, sizeof(*ss))))
        return FALSE;
    memset(ss, 0, sizeof(*ss));
    ss->mag_filter_nearest = t->mag_filter == GL_NEAREST;
    ss->min_filter_nearest = t->min_filter == GL_NEAREST || t->min_filter == GL_NEAREST_MIPMAP_NEAREST ||
                             t->min_filter == GL_NEAREST_MIPMAP_LINEAR;
    ss->mip_filter_nearest = t->min_filter != GL_NEAREST_MIPMAP_LINEAR &&
                             t->min_filter != GL_LINEAR_MIPMAP_LINEAR;
    ss->depth_compare_function = V3D_COMPARE_FUNC_NEVER;
    ss->wrap_s = t->wrap_s == GL_CLAMP_TO_EDGE ? V3D_WRAP_MODE_CLAMP
               : t->wrap_s == GL_MIRRORED_REPEAT ? V3D_WRAP_MODE_MIRROR : V3D_WRAP_MODE_REPEAT;
    ss->wrap_t = t->wrap_t == GL_CLAMP_TO_EDGE ? V3D_WRAP_MODE_CLAMP
               : t->wrap_t == GL_MIRRORED_REPEAT ? V3D_WRAP_MODE_MIRROR : V3D_WRAP_MODE_REPEAT;
    /* Mesa: without mipmapping the LOD is clamped just above the base
     * level, so the hardware still chooses between the min and mag filters. */
    ss->min_level_of_detail = V3D_FLOAT_TO_U4_8(0.f);
    ss->max_level_of_detail = mip ? V3D_FLOAT_TO_U4_8((float)(levels - 1)) : V3D_FLOAT_TO_U4_8(1.f / 256.f);
    swivel24 = (UBYTE *)ss;
    temp = swivel24[3];
    swivel24[3] = swivel24[1];
    swivel24[1] = temp;
    swivel16 = (UWORD *)ss;
    swivel16[3] = LE16(swivel16[3]);
    *sampler_state = (ULONG)ss;
    return TRUE;
}
