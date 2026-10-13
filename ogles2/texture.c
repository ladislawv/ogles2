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
#include "v3d_tfu.h"
#include "v3d_gpu_lock.h"
#include "etc1.h"

extern v3d_u32 g_v3d_cl_generation; /* v3d_frame.c: bumped whenever a control list restarts */

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
    ULONG blank;                    /* levels never given texels (glTexImage2D without pixels):
                                     * the copy is zeros, as a new GPU copy is (MEMF_CLEAR) */
};

struct OGLES2Texture
{
    GLuint name;
    GLenum target;                  /* 0 until first bound */
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    GLfloat max_aniso;              /* EXT_texture_filter_anisotropic: 1 = off */
    GLfloat lod_bias;               /* EXT_texture_lod_bias (a texture parameter, as AmigaOS 4's) */
    GLfloat min_lod, max_lod;       /* SGIS_texture_lod */
    GLint base_level, max_level;
    ULONG pver;                     /* bumped by every parameter change: the records' check */
    struct OGLES2Face f[6];         /* 2D: f[0]; a cube map: +X, -X, +Y, -Y, +Z, -Z */
    V3DTexture *gpu;                /* NULL until level 0 exists (a cube map: of all faces) */
    UBYTE hw_type, cpp, swizzle[4]; /* the stored format (storage_of), set with gpu */
    ULONG face_stride;              /* a cube map: bytes from one face's levels to the next */
    ULONG used_serial;              /* the job that last drew with it */
    BOOL warned_incomplete;         /* sampled while incomplete: logged once */
    BOOL warned_filter;             /* sampled nearest though linear was asked for: logged once */
    /* The state records last made for a draw (ogles2_texture_state), reused
     * while what they hold is the same: by every later job when they are in
     * the texture's own block (rec), else by the job's later draws (in its
     * state buffer: a job still used the block when they changed). */
    ULONG st_gen;                   /* the control list they are in (g_v3d_cl_generation) */
    APTR st_mem;                    /* the GPU copy they point at */
    ULONG st_pver;                  /* the parameters they were made from */
    ULONG st_tex, st_smp;
    struct OGLES2Context *st_ctx;   /* whose job's state buffer they are in (not st_own) */
    BOOL st_own;                    /* they are in rec */
    v3d_mem rec;                    /* 64 bytes: texture state at 0, sampler state at 32; goes
                                     * with the GPU copy (freed, or deferred when renamed) */
    /* GLES 2.0 4.4.3: deleting a texture detaches it from the deleting
     * context's bound framebuffer only; attached to another, it lives on
     * without its name (orphan) until the last attachment goes. */
    UWORD fbo_refs;                 /* framebuffer attachment points that refer to it */
    BOOL orphan;                    /* deleted while attached: freed with the last of them */
};

/* Both records fit their halves of rec. */
typedef char ogles2_rec_fits[sizeof(v3d_texture_shader_state) <= 32 && sizeof(v3d_sampler_state) <= 32 ? 1 : -1];

/* ---- the context's texture state ---- */

struct OGLES2TexState
{
    struct OGLES2Share *sh;         /* the named textures: sh->tex, sh->ntex (share.c) */
    struct OGLES2Texture default2d, defaultcube, defaultrect; /* name 0: the context's own */
    struct OGLES2Texture *unit2d[OGLES2_MAX_TEXTURE_UNITS], *unitcube[OGLES2_MAX_TEXTURE_UNITS];
    struct OGLES2Texture *unitrect[OGLES2_MAX_TEXTURE_UNITS]; /* ARB_texture_rectangle */
    struct OGLES2Texture fallback;  /* 1x1 (0,0,0,1): what an incomplete texture samples */
    UBYTE active;
    BOOL warned_flush;
    BOOL logged_tfu;                /* glGenerateMipmap's first TFU job logged */
};

static void sync(struct OGLES2Texture *t);
static void rect_defaults(struct OGLES2Texture *t);

static void init_object(struct OGLES2Texture *t, GLuint name, GLenum target)
{
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->target = target;
    t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    t->mag_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_REPEAT;
    t->max_aniso = 1.0f;
    t->min_lod = -1000.0f;
    t->max_lod = 1000.0f;
    t->max_level = 1000;
    rect_defaults(t);
}

/* ARB_texture_rectangle's defaults: a linear minification filter (no
 * mipmaps) and clamped wraps. Also when a new name is first bound there. */
static void rect_defaults(struct OGLES2Texture *t)
{
    if (t->target != GL_TEXTURE_RECTANGLE)
        return;
    t->min_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_CLAMP_TO_EDGE;
}

static BOOL cpu_uploads; /* probe U of the context last seen here (a test switch): store_tfu */

static struct OGLES2TexState *ts_get(struct OGLES2Context *ctx)
{
    struct OGLES2TexState *ts = ctx->tex;
    int i;
    cpu_uploads = (ctx->probe & OGLES2_PROBE_CPU_UPLOAD) != 0;
    if (ts)
        return ts;
    if (!(ts = AllocVec(sizeof(*ts), MEMF_ANY | MEMF_CLEAR)))
        return 0;
    ts->sh = ctx->share;
    init_object(&ts->default2d, 0, GL_TEXTURE_2D);
    init_object(&ts->defaultcube, 0, GL_TEXTURE_CUBE_MAP);
    init_object(&ts->defaultrect, 0, GL_TEXTURE_RECTANGLE);
    for (i = 0; i < OGLES2_MAX_TEXTURE_UNITS; i++) {
        ts->unit2d[i] = &ts->default2d;
        ts->unitcube[i] = &ts->defaultcube;
        ts->unitrect[i] = &ts->defaultrect;
    }
    ctx->tex = ts;
    return ts;
}

/* No job uses the GPU copy any more (before_change renamed it or rendered
 * the job): the copy and the records' block. */
static void free_gpu(struct OGLES2Texture *t)
{
    if (t->gpu) {
        if (t->gpu->texture_mem.hostptr)
            v3d_texture_free(ogles2_device(), t->gpu);
        FreeVec(t->gpu);
        t->gpu = 0;
    }
    if (t->rec.hostptr)
        v3d_mem_free(ogles2_device(), &t->rec);
    t->st_own = FALSE;
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
        t->f[k].blank = 0;
    }
    free_gpu(t);
}

/* aglDestroyContext (its jobs finished): the context's own textures; the
 * named ones with the share group's last member. */
void ogles2_textures_free(struct OGLES2Context *ctx)
{
    struct OGLES2TexState *ts = ctx->tex;
    struct OGLES2Share *sh = ctx->share;
    GLuint i;
    if (sh && sh->members == 1) {
        for (i = 1; i < sh->ntex; i++)
            if (sh->tex[i]) {
                free_levels(sh->tex[i]);
                FreeVec(sh->tex[i]);
            }
        if (sh->tex)
            FreeVec(sh->tex);
        sh->tex = 0;
        sh->ntex = 0;
        ogles2_names_free(&sh->far_tex);
    }
    if (!ts)
        return;
    free_levels(&ts->default2d);
    free_levels(&ts->defaultcube);
    free_levels(&ts->defaultrect);
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
    struct OGLES2Context *m, *user;
    V3DContext *v;
    /* Its records are made again at the next draw: a new level can change
     * the level count without a new GPU copy. */
    t->st_own = FALSE;
    t->st_gen = 0;
    /* An open job draws into it (a framebuffer object's colour image), any
     * member's of the share group: rendered first. Then the RAM copies are
     * made current. */
    for (m = ctx->share->first; m; m = m->share_next)
        ogles2_fbo_texture_changing(m, t);
    sync(t);
    /* A job not rendered yet uses it: an open one, or one submitted and
     * still running (M6 overlap), of any member: the old copy goes with
     * that job (share.c: serials are unique across contexts). */
    if (!t->used_serial || !(user = ogles2_job_user(ctx, t->used_serial)) || !(v = user->v3d))
        return;
    if (t->gpu && v->frame.retire_count < V3D_RETIRE_MAX - 8) {
        if (t->gpu->texture_mem.hostptr)
            v3d_frame_defer_free(&v->frame, &t->gpu->texture_mem);
        FreeVec(t->gpu);
        t->gpu = 0;
        if (t->rec.hostptr) { /* the job's draws read their records there too */
            v3d_frame_defer_free(&v->frame, &t->rec);
            memset(&t->rec, 0, sizeof(t->rec));
        }
        t->st_own = FALSE;
        t->used_serial = 0;
        ctx->n_renames++;
        if (!ts->warned_flush) {
            ts->warned_flush = TRUE;
            ogles2_log("texture %ld changed after a draw used it in the same frame: renamed "
                       "(logged once)", (ULONG)t->name);
        }
        return;
    }
    ogles2_log("texture %ld changed after a draw used it: the frame is rendered first "
               "(too many renamed blocks in this frame)", (ULONG)t->name);
    ogles2_job_flush(user, FALSE);
    ogles2_job_finish(user);
}

/* ---- names and binding ---- */

/* Makes `name` a valid index of the name table. */
static BOOL table_room(struct OGLES2TexState *ts, GLuint name)
{
    GLuint nn, j;
    struct OGLES2Texture **tab;
    if (name < ts->sh->ntex)
        return TRUE;
    nn = ts->sh->ntex ? ts->sh->ntex : 32;
    while (nn <= name)
        nn *= 2;
    if (!(tab = AllocVec(nn * sizeof(*tab), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    for (j = 0; j < ts->sh->ntex; j++)
        tab[j] = ts->sh->tex[j];
    if (ts->sh->tex)
        FreeVec(ts->sh->tex);
    ts->sh->tex = tab;
    ts->sh->ntex = nn;
    return TRUE;
}

/* A new texture object named `name` (no object has it). */
static struct OGLES2Texture *create(struct OGLES2TexState *ts, GLuint name)
{
    struct OGLES2Texture *t;
    GLuint slot = ogles2_name_slot(&ts->sh->far_tex, name, TRUE);
    if (slot == OGLES2_NO_SLOT || !table_room(ts, slot) ||
        !(t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR))) {
        ogles2_name_forget(&ts->sh->far_tex, name);
        return 0;
    }
    init_object(t, name, 0);
    ts->sh->tex[slot] = t;
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
        while (i < ts->sh->ntex && ts->sh->tex[i])
            i++;
        if (i >= OGLES2_NEAR_NAMES || !create(ts, i)) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        if (textures)
            textures[k] = i;
    }
}

static struct OGLES2Texture *by_name(struct OGLES2TexState *ts, GLuint name)
{
    GLuint slot = name ? ogles2_name_slot(&ts->sh->far_tex, name, FALSE) : OGLES2_NO_SLOT;
    return slot < ts->sh->ntex ? ts->sh->tex[slot] : 0;
}

/* A deleted texture's memory goes (its name already has). */
static void destroy(struct OGLES2Context *ctx, struct OGLES2TexState *ts, struct OGLES2Texture *t)
{
    int u;
    for (u = 0; u < 6; u++)
        t->f[u].rendered = 0; /* going: nothing to read back (SDL's render targets: 38 ms) */
    before_change(ctx, ts, t); /* a queued draw must not read it; a job drawing into it rendered */
    free_levels(t);
    FreeVec(t);
}

void ogles2_glDeleteTextures(struct OGLES2Context *ctx, GLsizei n, const GLuint *textures)
{
    struct OGLES2TexState *ts;
    struct OGLES2Context *m;
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
    ogles2_trace(ctx, "glDeleteTextures", n, n ? textures[0] : 0);
    for (k = 0; k < n; k++) {
        struct OGLES2Texture *t = by_name(ts, textures[k]);
        if (!t)
            continue;
        /* Detached from this context's bound framebuffer (its job drawing
         * into it rendered first); other framebuffers keep it (GLES 2.0
         * 4.4.3). Every member of the share group unbinds it. */
        ogles2_fbo_texture_deleted(ctx, t);
        for (m = ctx->share->first; m; m = m->share_next) {
            struct OGLES2TexState *mts = m->tex;
            if (!mts)
                continue;
            for (u = 0; u < OGLES2_MAX_TEXTURE_UNITS; u++) { /* deleting unbinds (GLES 2.0 3.7.13) */
                if (mts->unit2d[u] == t)
                    mts->unit2d[u] = &mts->default2d;
                if (mts->unitcube[u] == t)
                    mts->unitcube[u] = &mts->defaultcube;
                if (mts->unitrect[u] == t)
                    mts->unitrect[u] = &mts->defaultrect;
            }
        }
        ts->sh->tex[ogles2_name_slot(&ts->sh->far_tex, t->name, FALSE)] = 0;
        ogles2_name_forget(&ts->sh->far_tex, t->name);
        if (t->fbo_refs)
            t->orphan = TRUE; /* still attached: lives on, nameless */
        else
            destroy(ctx, ts, t);
    }
}

/* fbo.c: an attachment point refers to t, or no longer does. An orphan goes
 * with its last attachment; at aglDestroyContext (`now`: the jobs are
 * finished) without waiting for jobs. */
void ogles2_texture_attach(struct OGLES2Texture *t)
{
    t->fbo_refs++;
}

void ogles2_texture_detach(struct OGLES2Context *ctx, struct OGLES2Texture *t, BOOL now)
{
    struct OGLES2TexState *ts;
    if (t->fbo_refs)
        t->fbo_refs--;
    if (t->fbo_refs || !t->orphan)
        return;
    if (now || !(ts = ts_get(ctx))) {
        free_levels(t);
        FreeVec(t);
        return;
    }
    destroy(ctx, ts, t);
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
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP && target != GL_TEXTURE_RECTANGLE) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!texture) {
        t = target == GL_TEXTURE_2D ? &ts->default2d
          : target == GL_TEXTURE_CUBE_MAP ? &ts->defaultcube : &ts->defaultrect;
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
        if (!t->target) {
            t->target = target;
            rect_defaults(t);
        }
    }
    if (target == GL_TEXTURE_2D)
        ts->unit2d[ts->active] = t;
    else if (target == GL_TEXTURE_CUBE_MAP)
        ts->unitcube[ts->active] = t;
    else
        ts->unitrect[ts->active] = t;
}

/* ---- parameters ---- */

static struct OGLES2Texture *bound(struct OGLES2Context *ctx, struct OGLES2TexState *ts, GLenum target)
{
    if (target == GL_TEXTURE_2D)
        return ts->unit2d[ts->active];
    if (target == GL_TEXTURE_CUBE_MAP)
        return ts->unitcube[ts->active];
    if (target == GL_TEXTURE_RECTANGLE)
        return ts->unitrect[ts->active];
    ogles2_error(ctx, GL_INVALID_ENUM);
    return 0;
}

/* The largest anisotropy (the sampler's 2, 4, 8 or 16) and LOD bias (its
 * s8.8 field holds more; Mesa's v3d screen offers 15). */
#define MAX_ANISOTROPY 16.0f
#define MAX_LOD_BIAS   15.0f

/* A parameter given as `i` (glTexParameteri*) or `f` (glTexParameterf*):
 * enums and levels read i, the float parameters f (GL converts either way). */
static void tex_param(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLint i, GLfloat f)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    GLenum e = (GLenum)i;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)) || !(t = bound(ctx, ts, target)))
        return;
    /* ARB_texture_rectangle: no mipmapping, clamped coordinates only (of
     * desktop GL's three, the one GLES2 has), base level 0. */
    if (target == GL_TEXTURE_RECTANGLE) {
        if ((pname == GL_TEXTURE_MIN_FILTER && e != GL_NEAREST && e != GL_LINEAR) ||
            ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) && e != GL_CLAMP_TO_EDGE)) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            return;
        }
        if (pname == GL_TEXTURE_BASE_LEVEL && i != 0) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    }
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        if (e != GL_NEAREST && e != GL_LINEAR && e != GL_NEAREST_MIPMAP_NEAREST &&
            e != GL_LINEAR_MIPMAP_NEAREST && e != GL_NEAREST_MIPMAP_LINEAR && e != GL_LINEAR_MIPMAP_LINEAR)
            break;
        t->min_filter = e;
        t->pver++;
        return;
    case GL_TEXTURE_MAG_FILTER:
        if (e != GL_NEAREST && e != GL_LINEAR)
            break;
        t->mag_filter = e;
        t->pver++;
        return;
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        if (e != GL_REPEAT && e != GL_CLAMP_TO_EDGE && e != GL_MIRRORED_REPEAT && e != GL_MIRROR_CLAMP_TO_EDGE)
            break;
        if (pname == GL_TEXTURE_WRAP_S)
            t->wrap_s = e;
        else
            t->wrap_t = e;
        t->pver++;
        return;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT:
        if (!(f >= 1.0f)) { /* EXT_texture_filter_anisotropic: below 1 (or NaN) */
            ogles2_error(ctx, GL_INVALID_VALUE);
            return;
        }
        t->max_aniso = f > MAX_ANISOTROPY ? MAX_ANISOTROPY : f;
        t->pver++;
        return;
    case GL_TEXTURE_LOD_BIAS:
        t->lod_bias = f; /* clamped where it is used (GL keeps the value) */
        t->pver++;
        return;
    case GL_TEXTURE_MIN_LOD:
        t->min_lod = f;
        t->pver++;
        return;
    case GL_TEXTURE_MAX_LOD:
        t->max_lod = f;
        t->pver++;
        return;
    case GL_TEXTURE_BASE_LEVEL:
    case GL_TEXTURE_MAX_LEVEL:
        if (i < 0) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return;
        }
        if (pname == GL_TEXTURE_BASE_LEVEL)
            t->base_level = i;
        else
            t->max_level = i;
        t->pver++;
        return;
    default:
        break;
    }
    ogles2_error(ctx, GL_INVALID_ENUM);
}

/* GL's conversion of a float to an integer parameter: the nearest. */
static GLint round_param(GLfloat f)
{
    return f < 0.0f ? -(GLint)(0.5f - f) : (GLint)(f + 0.5f);
}

void ogles2_glTexParameteri(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLint param)
{
    tex_param(ctx, target, pname, param, (GLfloat)param);
}

void ogles2_glTexParameterf(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLfloat param)
{
    tex_param(ctx, target, pname, round_param(param), param);
}

void ogles2_glTexParameteriv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                             const GLint *params)
{
    if (params)
        tex_param(ctx, target, pname, params[0], (GLfloat)params[0]);
}

void ogles2_glTexParameterfv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                             const GLfloat *params)
{
    if (params)
        tex_param(ctx, target, pname, round_param(params[0]), params[0]);
}

/* A parameter's value, as a float; FALSE (GL_INVALID_ENUM set) if pname is none. */
static BOOL get_param(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLfloat *v)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)) || !(t = bound(ctx, ts, target)))
        return FALSE;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:          *v = (GLfloat)t->min_filter; return TRUE;
    case GL_TEXTURE_MAG_FILTER:          *v = (GLfloat)t->mag_filter; return TRUE;
    case GL_TEXTURE_WRAP_S:              *v = (GLfloat)t->wrap_s; return TRUE;
    case GL_TEXTURE_WRAP_T:              *v = (GLfloat)t->wrap_t; return TRUE;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT:  *v = t->max_aniso; return TRUE;
    case GL_TEXTURE_LOD_BIAS:            *v = t->lod_bias; return TRUE;
    case GL_TEXTURE_MIN_LOD:             *v = t->min_lod; return TRUE;
    case GL_TEXTURE_MAX_LOD:             *v = t->max_lod; return TRUE;
    case GL_TEXTURE_BASE_LEVEL:          *v = (GLfloat)t->base_level; return TRUE;
    case GL_TEXTURE_MAX_LEVEL:           *v = (GLfloat)t->max_level; return TRUE;
    default:
        ogles2_error(ctx, GL_INVALID_ENUM);
        return FALSE;
    }
}

void ogles2_glGetTexParameteriv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                GLint *params)
{
    GLfloat v;
    if (params && get_param(ctx, target, pname, &v))
        *params = pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER ||
                  pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T ||
                  pname == GL_TEXTURE_BASE_LEVEL || pname == GL_TEXTURE_MAX_LEVEL
                      ? (GLint)(ULONG)v /* enums and levels: exact */
                      : round_param(v);
}

void ogles2_glGetTexParameterfv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                GLfloat *params)
{
    GLfloat v;
    if (params && get_param(ctx, target, pname, &v))
        *params = v;
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
    case GL_TEXTURE_BINDING_RECTANGLE: *v = ts ? (GLint)ts->unitrect[ts->active]->name : 0; return TRUE;
    case GL_MAX_RECTANGLE_TEXTURE_SIZE: *v = MAX_SIZE; return TRUE;
    case GL_UNPACK_ALIGNMENT:     *v = ctx->unpack_alignment; return TRUE;
    case GL_PACK_ALIGNMENT:       *v = ctx->pack_alignment; return TRUE;
    case GL_MAX_TEXTURE_SIZE:
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: *v = MAX_SIZE; return TRUE;
    case GL_MAX_TEXTURE_IMAGE_UNITS: *v = OGLES2_MAX_STAGE_SAMPLERS; return TRUE;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *v = OGLES2_MAX_TEXTURE_UNITS; return TRUE;
    /* Vertex texture fetch: the draws bind the units of both stages' samplers
     * (draw.c texture_states); dEQP-GLES2's texture.vertex tests sample in
     * the vertex shader (M7). Units are shared between the stages. */
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: *v = OGLES2_MAX_STAGE_SAMPLERS; return TRUE;
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: *v = (GLint)MAX_ANISOTROPY; return TRUE;
    case GL_MAX_TEXTURE_LOD_BIAS: *v = (GLint)MAX_LOD_BIAS; return TRUE;
    default:                      return FALSE;
    }
}

/* ---- images ---- */

/* What a level's RAM copy holds, by its format and type:
 *  - RGBA8: RGBA bytes, GL's conversions (the 16-bit packings expanded);
 *  - FLOAT: RGBA floats, the 68k's own (OES_texture_float, _half_float);
 *  - DEPTH16, DEPTH24: the stored depth word as a ULONG (OES_depth_texture:
 *    16 bits; or 24 bits << 8 with the stencil in the low byte, which is
 *    also OES_packed_depth_stencil's UNSIGNED_INT_24_8 layout);
 *  - BLOCKS: S3TC's 4x4 blocks as given, rows of ceil(w/4) blocks (M7,
 *    docs/M7_EXTENSIONS_PLAN.md step 3), stored as they are: the TMU
 *    decodes them.
 * The levels of a texture must be of level 0's class to be sampled. */
#define CLASS_RGBA8   0
#define CLASS_FLOAT   1
#define CLASS_DEPTH16 2
#define CLASS_DEPTH24 3
#define CLASS_BLOCKS  4

/* EXT_texture_compression_dxt1, EXT_texture_compression_s3tc and ANGLE's
 * dxt3/dxt5 (the same enums): a block's bytes, 0 for another format. */
static ULONG dxt_block_bytes(GLenum format)
{
    switch (format) {
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
        return 8;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        return 16;
    default:
        return 0;
    }
}

/* Texels in blocks of 4, rounded up (a level smaller than 4x4 is one block). */
static ULONG blocks4(ULONG texels)
{
    return (texels + 3) / 4;
}

static int class_of(GLenum format, GLenum type)
{
    if (dxt_block_bytes(format))
        return CLASS_BLOCKS;
    if (type == GL_FLOAT || type == GL_HALF_FLOAT_OES)
        return CLASS_FLOAT;
    if (format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_SHORT)
        return CLASS_DEPTH16;
    if (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES)
        return CLASS_DEPTH24;
    return CLASS_RGBA8;
}

/* Bytes per texel of a RAM copy of that class (BLOCKS is never asked: its
 * copies are only stored and replaced, never converted, filtered or read). */
static ULONG copy_bytes(int cls)
{
    return cls == CLASS_FLOAT ? 16 : 4;
}

static int level_class(const struct OGLES2Face *f, int l)
{
    return class_of(f->format[l], f->type[l]);
}

/* Set once an image whose texture the TMU returns 32 bits for (32-bit
 * floats, depth) was given: from then on draws look at their samplers'
 * textures to pick the program's 32-bit variants (draw.c). */
ULONG ogles2_wide_seen;

static ULONG wide_class(GLenum format, GLenum type)
{
    return type == GL_FLOAT || format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES;
}

/* Bytes per pixel of a valid format/type pair; 0 and the error to set if not. */
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1 /* EXT_texture_format_BGRA8888 */
#endif
#ifndef GL_BGRA8_EXT
#define GL_BGRA8_EXT 0x93A1
#endif

static int components(GLenum format)
{
    switch (format) {
    case GL_RGBA:            return 4;
    case GL_RGB:             return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_LUMINANCE:
    case GL_ALPHA:           return 1;
    default:                 return 0;
    }
}

static int pixel_size(GLenum format, GLenum type, GLenum *err)
{
    *err = GL_NO_ERROR;
    /* OES_depth_texture and OES_packed_depth_stencil's texture formats take
     * their types only, and their types no other format. */
    if (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES ||
        type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_24_8_OES) {
        if (format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_SHORT) return 2;
        if (format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_INT) return 4;
        if (format == GL_DEPTH_STENCIL_OES && type == GL_UNSIGNED_INT_24_8_OES) return 4;
        *err = (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES || components(format) ||
                format == GL_BGRA_EXT) &&
               (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_24_8_OES ||
                type == GL_UNSIGNED_BYTE || type == GL_FLOAT || type == GL_HALF_FLOAT_OES ||
                type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
                type == GL_UNSIGNED_SHORT_5_5_5_1)
                   ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    }
    switch (type) {
    case GL_FLOAT:          /* OES_texture_float */
    case GL_HALF_FLOAT_OES: /* OES_texture_half_float */
        if (components(format))
            return components(format) * (type == GL_FLOAT ? 4 : 2);
        *err = format == GL_BGRA_EXT ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    }
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

/* A float's bits and back. */
static ULONG f2u(GLfloat f)
{
    union { GLfloat f; ULONG u; } x;
    x.f = f;
    return x.u;
}

static GLfloat u2f(ULONG u)
{
    union { GLfloat f; ULONG u; } x;
    x.u = u;
    return x.f;
}

/* IEEE half precision (OES_texture_half_float) to single and back, by
 * their bits (nearest, ties away from zero). */
static GLfloat half_to_float(UWORD h)
{
    ULONG s = (ULONG)(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 1023;
    if (e == 31)
        return u2f(s | 0x7f800000UL | (m << 13)); /* infinity, NaN */
    if (!e) { /* zero, or a subnormal: m * 2^-24 */
        GLfloat v = (GLfloat)m * (1.0f / 16777216.0f);
        return s ? -v : v;
    }
    return u2f(s | ((e + 112) << 23) | (m << 13));
}

static UWORD float_to_half(GLfloat f)
{
    ULONG u = f2u(f), s = (u >> 16) & 0x8000, m = u & 0x7fffff;
    LONG e = (LONG)((u >> 23) & 255) - 127 + 15;
    if (((u >> 23) & 255) == 255)
        return (UWORD)(s | 0x7c00 | (m ? 0x200 : 0));
    if (e >= 31)
        return (UWORD)(s | 0x7c00); /* too large: infinity */
    if (e <= 0) {                   /* a subnormal half, or zero */
        ULONG shift, h;
        if (e < -10)
            return (UWORD)s;
        m |= 0x800000;
        shift = (ULONG)(14 - e);
        h = (m >> shift) + ((m >> (shift - 1)) & 1);
        return (UWORD)(s | h);
    }
    return (UWORD)((s | ((ULONG)e << 10) | (m >> 13)) + ((m >> 12) & 1)); /* a carry rounds the exponent up */
}

/* Floats or half floats of the application's format into RGBA floats (the
 * conventions of the RGBA8 copy: LUMINANCE (L,L,L,1), ALPHA (0,0,0,A)). */
static void convert_float(UBYTE *dst, ULONG dst_stride, const UBYTE *src, GLenum format, GLenum type,
                          int w, int h, int align)
{
    int n = components(format), bpc = type == GL_FLOAT ? 4 : 2, x, y, c;
    ULONG src_stride = ((ULONG)(w * n * bpc) + align - 1) & ~(ULONG)(align - 1);
    for (y = 0; y < h; y++, src += src_stride, dst += dst_stride) {
        const UBYTE *s = src;
        GLfloat *d = (GLfloat *)dst;
        for (x = 0; x < w; x++, d += 4) {
            GLfloat v[4];
            for (c = 0; c < n; c++, s += bpc) {
                if (bpc == 4) {
                    memcpy(&v[c], s, 4); /* the 68k's own byte order, as GL's types are */
                } else {
                    UWORD hv;
                    memcpy(&hv, s, 2);
                    v[c] = half_to_float(hv);
                }
            }
            switch (format) {
            case GL_RGBA:            d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = v[3]; break;
            case GL_RGB:             d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = 1.0f; break;
            case GL_LUMINANCE_ALPHA: d[0] = d[1] = d[2] = v[0]; d[3] = v[1]; break;
            case GL_LUMINANCE:       d[0] = d[1] = d[2] = v[0]; d[3] = 1.0f; break;
            default: /* GL_ALPHA */  d[0] = d[1] = d[2] = 0.0f; d[3] = v[0]; break;
            }
        }
    }
}

/* Depth values into the stored depth word (CLASS_DEPTH16/24). */
static void convert_depth(UBYTE *dst, ULONG dst_stride, const UBYTE *src, GLenum type, int w, int h,
                          int align)
{
    int bpp = type == GL_UNSIGNED_SHORT ? 2 : 4, x, y;
    ULONG src_stride = ((ULONG)(w * bpp) + align - 1) & ~(ULONG)(align - 1);
    for (y = 0; y < h; y++, src += src_stride, dst += dst_stride) {
        const UBYTE *s = src;
        ULONG *d = (ULONG *)dst;
        for (x = 0; x < w; x++, s += bpp) {
            if (bpp == 2) {
                UWORD v;
                memcpy(&v, s, 2);
                d[x] = v;
            } else {
                ULONG v;
                memcpy(&v, s, 4);
                /* 24 bits kept; UNSIGNED_INT_24_8's low byte is the stencil */
                d[x] = type == GL_UNSIGNED_INT ? v & 0xffffff00UL : v;
            }
        }
    }
}

/* The 16-bit packed types into an RGBA8 texel (R in the top byte: the copy's
 * byte order on the 68k) as two table lookups, one per byte, ORed: each
 * channel's expansion (x5, x6, x4) splits into bits from the high byte and
 * bits from the low byte that never overlap. [0] by the high byte, [1] by
 * the low. Made once, 6 KB. */
static ULONG t565[2][256], t4444[2][256], t5551[2][256];
static BOOL packed_tables;

static void make_packed_tables(void)
{
    int b;
    for (b = 0; b < 256; b++) {
        ULONG h = (ULONG)b, l = (ULONG)b;
        /* 5-6-5: R = h>>3; G = (h&7)<<3 | l>>5; B = l&31 */
        t565[0][b] = ((ULONG)x5((UWORD)(h >> 3)) << 24) | ((((h & 7) << 5) | ((h & 7) >> 1)) << 16) | 0xFF;
        t565[1][b] = (((l >> 5) << 2) << 16) | ((ULONG)x5((UWORD)(l & 31)) << 8);
        /* 4-4-4-4: R = h>>4; G = h&15; B = l>>4; A = l&15 */
        t4444[0][b] = ((ULONG)x4((UWORD)(h >> 4)) << 24) | ((ULONG)x4((UWORD)(h & 15)) << 16);
        t4444[1][b] = ((ULONG)x4((UWORD)(l >> 4)) << 8) | x4((UWORD)(l & 15));
        /* 5-5-5-1: R = h>>3; G = (h&7)<<2 | l>>6; B = (l>>1)&31; A = l&1 */
        t5551[0][b] = ((ULONG)x5((UWORD)(h >> 3)) << 24) | ((((h & 7) << 5) | (h & 7)) << 16);
        t5551[1][b] = (((l >> 6) << 3) << 16) | ((ULONG)x5((UWORD)((l >> 1) & 31)) << 8) | ((l & 1) ? 0xFF : 0);
    }
    packed_tables = TRUE;
}

/* One row of RGBA8-class texels from the application's (format, type), the
 * common cases each in a loop of its own (a switch per texel cost 50 ms a
 * megapixel on the A1200). FALSE for one convert's general loop does. */
static BOOL convert_row(ULONG *d, const UBYTE *s, GLenum format, GLenum type, int w)
{
    const ULONG (*tab)[256];
    int x;
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:   tab = (const ULONG (*)[256])t565; break;
    case GL_UNSIGNED_SHORT_4_4_4_4: tab = (const ULONG (*)[256])t4444; break;
    case GL_UNSIGNED_SHORT_5_5_5_1: tab = (const ULONG (*)[256])t5551; break;
    case GL_UNSIGNED_BYTE:          tab = 0; break;
    default:                        return FALSE;
    }
    if (tab) {
        if (!packed_tables)
            make_packed_tables();
        for (x = 0; x < w; x++, s += 2) /* big-endian: the 68k's own order */
            d[x] = tab[0][s[0]] | tab[1][s[1]];
        return TRUE;
    }
    switch (format) {
    case GL_RGBA:
        memcpy(d, s, (ULONG)w * 4);
        return TRUE;
    case GL_BGRA_EXT:
        for (x = 0; x < w; x++, s += 4)
            d[x] = ((ULONG)s[2] << 24) | ((ULONG)s[1] << 16) | ((ULONG)s[0] << 8) | s[3];
        return TRUE;
    case GL_RGB:
        for (x = 0; x < w; x++, s += 3)
            d[x] = ((ULONG)s[0] << 24) | ((ULONG)s[1] << 16) | ((ULONG)s[2] << 8) | 0xFF;
        return TRUE;
    case GL_LUMINANCE_ALPHA:
        for (x = 0; x < w; x++, s += 2)
            d[x] = ((ULONG)s[0] * 0x01010100UL) | s[1];
        return TRUE;
    case GL_LUMINANCE:
        for (x = 0; x < w; x++)
            d[x] = ((ULONG)s[x] * 0x01010100UL) | 0xFF;
        return TRUE;
    case GL_ALPHA:
        for (x = 0; x < w; x++)
            d[x] = s[x];
        return TRUE;
    }
    return FALSE;
}

/* w x h pixels of the application's (format, type) at src, rows aligned to
 * the unpack alignment, into the RAM copy at dst (dst_stride bytes per row)
 * in the class of (format, type). */
static void convert(UBYTE *dst, ULONG dst_stride, const UBYTE *src, GLenum format, GLenum type,
                    int w, int h, int align)
{
    GLenum err;
    int bpp = pixel_size(format, type, &err), x, y;
    ULONG src_stride = ((ULONG)(w * bpp) + align - 1) & ~(ULONG)(align - 1);
    switch (class_of(format, type)) {
    case CLASS_FLOAT:
        convert_float(dst, dst_stride, src, format, type, w, h, align);
        return;
    case CLASS_DEPTH16:
    case CLASS_DEPTH24:
        convert_depth(dst, dst_stride, src, type, w, h, align);
        return;
    }
    if (!((ULONG)dst & 3) && !(dst_stride & 3) && convert_row((ULONG *)dst, src, format, type, 0)) {
        for (y = 0; y < h; y++, src += src_stride, dst += dst_stride)
            convert_row((ULONG *)dst, src, format, type, w);
        return;
    }
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

/* Level `l` of a face has the size the mip chain of its level 0 gives it,
 * and its RAM copy is of level 0's class (the stored format is level 0's). */
static BOOL consistent(const struct OGLES2Face *f, int l)
{
    int w = f->w[0] >> l, h = f->h[0] >> l;
    /* The same format and type as level 0 (GLES 2.0 3.7.10), not just the
     * same storage: RGB and RGBA levels are both kept as RGBA8. */
    if (!(f->defined & (1UL << l)) || !(f->defined & 1) || level_class(f, l) != level_class(f, 0) ||
        f->format[l] != f->format[0] || f->type[l] != f->type[0])
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
        if (!(t->f[k].defined & 1) || t->f[k].w[0] != f0->w[0] || t->f[k].h[0] != f0->h[0] ||
            level_class(&t->f[k], 0) != level_class(f0, 0) || t->f[k].format[0] != f0->format[0] ||
            t->f[k].type[0] != f0->type[0])
            return FALSE;
    return faces(t) == 1 || f0->w[0] == f0->h[0];
}

static BOOL mipmapped(GLenum min_filter)
{
    return min_filter != GL_NEAREST && min_filter != GL_LINEAR;
}

/* GLES 2.0 3.7.10 with SGIS_texture_lod's levels: level 0 (a cube map's
 * cube complete; the GPU copy is laid out from it, so it must exist even
 * when a later base level is sampled), and the levels sampled, base_level
 * up to max_level or the 1x1 level (a mipmapping filter) or base_level
 * alone, consistent on every face and in the GPU copy: *first and *last. */
static BOOL complete(const struct OGLES2Texture *t, int *first, int *last)
{
    int n, l, k, b = t->base_level, e;
    if (!t->gpu || !base_ok(t))
        return FALSE;
    n = level_count(t->f[0].w[0], t->f[0].h[0]);
    if (b >= n)
        return FALSE;
    e = !mipmapped(t->min_filter) ? b : t->max_level < n - 1 ? t->max_level : n - 1;
    if (e < b || e >= t->gpu->num_levels)
        return FALSE;
    for (k = 0; k < faces(t); k++)
        for (l = b; l <= e; l++)
            if (l && !consistent(&t->f[k], l))
                return FALSE;
    *first = b;
    *last = e;
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
    if (dxt_block_bytes(f)) {
        /* S3TC: BC1-BC3, a block a "texel" (Mesa's table: DXT1 RGB is XYZ1) */
        type = f == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT   ? V3D_TEXTURE_DATA_FORMAT_BC2
             : f == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT   ? V3D_TEXTURE_DATA_FORMAT_BC3
                                                       : V3D_TEXTURE_DATA_FORMAT_BC1;
        cpp = (UBYTE)dxt_block_bytes(f);
        if (f == GL_COMPRESSED_RGB_S3TC_DXT1_EXT)
            sw[3] = O;
    } else if (ty == GL_FLOAT || ty == GL_HALF_FLOAT_OES) {
        /* OES_texture_float (32-bit returns: the .v3dw variants) and
         * OES_texture_half_float (16-bit, filterable); RGB as RGBA. */
        BOOL h = ty == GL_HALF_FLOAT_OES;
        if (f == GL_LUMINANCE || f == GL_ALPHA) {
            type = h ? V3D_TEXTURE_DATA_FORMAT_R16F : V3D_TEXTURE_DATA_FORMAT_R32F;
            cpp = h ? 2 : 4;
            if (f == GL_LUMINANCE) {
                sw[0] = sw[1] = sw[2] = R; sw[3] = O;
            } else {
                sw[0] = sw[1] = sw[2] = Z; sw[3] = R;
            }
        } else if (f == GL_LUMINANCE_ALPHA) {
            type = h ? V3D_TEXTURE_DATA_FORMAT_RG16F : V3D_TEXTURE_DATA_FORMAT_RG32F;
            cpp = h ? 4 : 8;
            sw[0] = sw[1] = sw[2] = R; sw[3] = G;
        } else {
            type = h ? V3D_TEXTURE_DATA_FORMAT_RGBA16F : V3D_TEXTURE_DATA_FORMAT_RGBA32F;
            cpp = h ? 8 : 16;
        }
    } else if (f == GL_DEPTH_COMPONENT || f == GL_DEPTH_STENCIL_OES) {
        /* OES_depth_texture: sampled as luminance, (d, d, d, 1); 32-bit returns */
        type = ty == GL_UNSIGNED_SHORT ? V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP16 : V3D_TEXTURE_DATA_FORMAT_DEPTH24X8;
        cpp = ty == GL_UNSIGNED_SHORT ? 2 : 4;
        sw[0] = sw[1] = sw[2] = R; sw[3] = O;
    } else if (f == GL_RGB && ty == GL_UNSIGNED_SHORT_5_6_5) {
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

/* A 16-bit texel stored little-endian (the GPU's order) from the 68k. */
#define LE16_STORE(p, v) (*(UWORD *)(p) = (UWORD)(((v) << 8) | ((v) >> 8)))

/* n RGBA8 texels into the stored format, in the GPU's byte order (16-bit
 * texels little-endian, the GL packing: red in the top bits). Each format
 * in a loop of its own, a texel read as one long (R in the top byte): a
 * switch per texel cost 50 ms a megapixel on the A1200. The copy's rows and
 * `d` are word aligned (AllocVec, widths in whole texels). */
static void pack(const struct OGLES2Texture *t, const UBYTE *s, UBYTE *d, ULONG n)
{
    const ULONG *p = (const ULONG *)s;
    ULONG i, c;
    switch (t->hw_type) {
    case V3D_TEXTURE_DATA_FORMAT_RGB565:
        for (i = 0; i < n; i++, d += 2) {
            UWORD v;
            c = p[i];
            v = (UWORD)(((c >> 16) & 0xF800) | ((c >> 13) & 0x07E0) | ((c >> 11) & 0x001F));
            LE16_STORE(d, v);
        }
        return;
    case V3D_TEXTURE_DATA_FORMAT_RGBA4:
        for (i = 0; i < n; i++, d += 2) {
            UWORD v;
            c = p[i];
            v = (UWORD)(((c >> 16) & 0xF000) | ((c >> 12) & 0x0F00) | ((c >> 8) & 0x00F0) | ((c >> 4) & 0x000F));
            LE16_STORE(d, v);
        }
        return;
    case V3D_TEXTURE_DATA_FORMAT_RGB5A1:
        for (i = 0; i < n; i++, d += 2) {
            UWORD v;
            c = p[i];
            v = (UWORD)(((c >> 16) & 0xF800) | ((c >> 13) & 0x07C0) | ((c >> 10) & 0x003E) | ((c >> 7) & 1));
            LE16_STORE(d, v);
        }
        return;
    case V3D_TEXTURE_DATA_FORMAT_R8: /* luminance (red of the copy) or alpha */
        if (t->swizzle[3] == v3d_SWIZZLE_RED)
            for (i = 0; i < n; i++)
                d[i] = (UBYTE)p[i];
        else
            for (i = 0; i < n; i++)
                d[i] = (UBYTE)(p[i] >> 24);
        return;
    case V3D_TEXTURE_DATA_FORMAT_RG8: /* luminance, alpha */
        for (i = 0; i < n; i++, d += 2) {
            c = p[i];
            d[0] = (UBYTE)(c >> 24);
            d[1] = (UBYTE)c;
        }
        return;
    default:
        memcpy(d, s, n * 4);
        return;
    }
}

static BOOL float_type(UBYTE hw)
{
    return hw == V3D_TEXTURE_DATA_FORMAT_R32F || hw == V3D_TEXTURE_DATA_FORMAT_RG32F ||
           hw == V3D_TEXTURE_DATA_FORMAT_RGBA32F || hw == V3D_TEXTURE_DATA_FORMAT_R16F ||
           hw == V3D_TEXTURE_DATA_FORMAT_RG16F || hw == V3D_TEXTURE_DATA_FORMAT_RGBA16F;
}

static BOOL depth_type(UBYTE hw)
{
    return hw == V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP16 || hw == V3D_TEXTURE_DATA_FORMAT_DEPTH24X8;
}

/* Formats the TMU returns 32 bits a channel for (Mesa's v3dx_format_table.c):
 * a draw sampling one needs the program's .v3dp's 32-bit variants. */
static BOOL wide_type(UBYTE hw)
{
    return hw == V3D_TEXTURE_DATA_FORMAT_R32F || hw == V3D_TEXTURE_DATA_FORMAT_RG32F ||
           hw == V3D_TEXTURE_DATA_FORMAT_RGBA32F || depth_type(hw);
}

/* Formats the TMU cannot filter (Mesa's table): sampled nearest whatever the
 * filters say (OES_texture_float_linear is not offered; OES_depth_texture
 * leaves filtering to the implementation). */
static BOOL unfilterable_type(UBYTE hw)
{
    return wide_type(hw);
}

static void put_le32(UBYTE *d, ULONG v)
{
    d[0] = (UBYTE)v; d[1] = (UBYTE)(v >> 8); d[2] = (UBYTE)(v >> 16); d[3] = (UBYTE)(v >> 24);
}

/* n texels of a FLOAT or DEPTH RAM copy into the stored format, little-endian.
 * Luminance and alpha formats store red or alpha, as pack does. */
static void pack_wide(const struct OGLES2Texture *t, const UBYTE *src, UBYTE *d, ULONG n)
{
    ULONG i;
    BOOL alpha = t->swizzle[3] == v3d_SWIZZLE_RED; /* an ALPHA format: its one channel is alpha */
    if (depth_type(t->hw_type)) {
        const ULONG *s = (const ULONG *)src;
        for (i = 0; i < n; i++)
            if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP16) {
                *d++ = (UBYTE)s[i];
                *d++ = (UBYTE)(s[i] >> 8);
            } else {
                put_le32(d, s[i]);
                d += 4;
            }
        return;
    }
    for (i = 0; i < n; i++, src += 16) {
        const GLfloat *s = (const GLfloat *)src;
        GLfloat v[4];
        int c, nc;
        switch (t->hw_type) {
        case V3D_TEXTURE_DATA_FORMAT_R32F:
        case V3D_TEXTURE_DATA_FORMAT_R16F:
            v[0] = alpha ? s[3] : s[0];
            nc = 1;
            break;
        case V3D_TEXTURE_DATA_FORMAT_RG32F:
        case V3D_TEXTURE_DATA_FORMAT_RG16F:
            v[0] = s[0];
            v[1] = s[3];
            nc = 2;
            break;
        default:
            v[0] = s[0]; v[1] = s[1]; v[2] = s[2]; v[3] = s[3];
            nc = 4;
            break;
        }
        for (c = 0; c < nc; c++) {
            if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_R16F || t->hw_type == V3D_TEXTURE_DATA_FORMAT_RG16F ||
                t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA16F) {
                UWORD hv = float_to_half(v[c]);
                *d++ = (UBYTE)hv;
                *d++ = (UBYTE)(hv >> 8);
            } else {
                put_le32(d, f2u(v[c]));
                d += 4;
            }
        }
    }
}

/* The inverse of pack_wide for depth (what a framebuffer object's job drew). */
static void unpack_depth(const struct OGLES2Texture *t, const UBYTE *s, ULONG *d, ULONG n)
{
    ULONG i;
    for (i = 0; i < n; i++)
        if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP16) {
            d[i] = (ULONG)s[0] | ((ULONG)s[1] << 8);
            s += 2;
        } else {
            d[i] = (ULONG)s[0] | ((ULONG)s[1] << 8) | ((ULONG)s[2] << 16) | ((ULONG)s[3] << 24);
            s += 4;
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

/* A box of face k, level l from the GPU's copy, as the RAM copy holds it:
 * RGBA8 rows, or depth words (a depth texture a job drew into). Float
 * textures are never drawn into. */
static BOOL load_box(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h, UBYTE *rgba,
                     ULONG stride)
{
    V3DTexture tmp;
    ULONG row = (ULONG)w * t->cpp;
    UBYTE *packed;
    int j;
    if (!t->gpu || l >= t->gpu->num_levels || w <= 0 || h <= 0 || float_type(t->hw_type))
        return FALSE;
    if (!(packed = AllocVec(row * h + 4, MEMF_ANY)))
        return FALSE;
    v3d_texture_load(ogles2_device(), face_gpu(t, k, &tmp), (v3d_u8)l, packed, row, x, y, w, h);
    for (j = 0; j < h; j++)
        if (depth_type(t->hw_type))
            unpack_depth(t, packed + (ULONG)j * row, (ULONG *)(rgba + (ULONG)j * stride), (ULONG)w);
        else
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
            if ((f->rendered & (1UL << l)) && f->shadow[l]) {
                load_box(t, k, l, 0, 0, f->w[l], f->h[l], f->shadow[l],
                         (ULONG)f->w[l] * copy_bytes(level_class(f, l)));
                f->blank &= ~(1UL << l);
            }
        f->rendered = 0;
    }
}

/* A whole level stored by the TFU from a raster copy in the stored format
 * (`row` bytes a row), where it beats the CPU's tiling (v3d_texture_store),
 * the copy in memory the GPU reaches, a format the TFU takes. The job's
 * fixed cost is a cache clean, the lock and a few bridge writes, 0.3-0.5 ms
 * on the A1200, then about 1.9 ms a megapixel; the CPU tiles RGBA8 into UIF
 * directly at 5.6 ms a megapixel, other formats at about 40: so 512x512 and
 * up for RGBA8, 128x128 for the rest. The caller has dealt with the jobs
 * that use the texture (before_change), as for the CPU's store. Probe U
 * keeps the CPU's. FALSE: the CPU stores it. */
#define TFU_UPLOAD_MIN_TEXELS       (128UL * 128UL)
#define TFU_UPLOAD_MIN_TEXELS_RGBA8 (512UL * 512UL)
#define TFU_LOCK_TIMEOUT_MS 5000 /* job.c's GPU_LOCK_TIMEOUT_MS */
static BOOL store_tfu(struct OGLES2Texture *t, int k, int l, const UBYTE *raster, ULONG row, int w, int h)
{
    V3DTexture tmp, *g;
    ULONG len;
    int r;
    if (cpu_uploads ||
        (ULONG)w * (ULONG)h < (t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA8 ? TFU_UPLOAD_MIN_TEXELS_RGBA8
                                                                          : TFU_UPLOAD_MIN_TEXELS) ||
        !v3d_tfu_supports_mipmaps(t->hw_type) || !ogles2_device_gpu_reachable((ULONG)raster, row * (ULONG)h))
        return FALSE;
    g = face_gpu(t, k, &tmp);
    CacheClearE(0, ~0UL, CACRF_ClearD); /* the copy in memory, and no dirty line over the level */
    if (!v3d_gpu_lock(ogles2_device_gpu_lock(), TFU_LOCK_TIMEOUT_MS))
        return FALSE;
    r = v3d_tfu_store_raster(ogles2_device(), g, (v3d_u8)l, t->hw_type, (v3d_u16)w, (v3d_u16)h, raster, row);
    v3d_gpu_unlock(ogles2_device_gpu_lock());
    if (r == -2)
        ogles2_log("texture %ld: the TFU did not finish an upload (%ldx%ld): the CPU stores it",
                   (ULONG)t->name, (ULONG)w, (ULONG)h);
    if (r)
        return FALSE;
    /* The level's memory as the TFU left it: no CPU cache line from before
     * (a later glTexSubImage2D's partial store reads them). */
    len = g->texture_mem.size;
    CachePostDMA(g->texture_mem.hostptr, &len, 0);
    return TRUE;
}

/* A box of level l of face k, from the RAM copy `src` (`w` texels a row, of
 * level 0's class: complete() saw to it). S3TC: the box on block bounds,
 * `src` its blocks, stored as they are (the TFU takes no BC format). */
static void store_box(struct OGLES2Texture *t, int k, int l, const UBYTE *src, int x, int y, int w,
                      int h)
{
    V3DTexture tmp;
    ULONG row = (ULONG)w * t->cpp, in;
    UBYTE *packed;
    BOOL whole = x == 0 && y == 0 && w == t->f[k].w[l] && h == t->f[k].h[l];
    int j;
    if (class_of(t->f[0].format[0], t->f[0].type[0]) == CLASS_BLOCKS) {
        v3d_texture_store(ogles2_device(), face_gpu(t, k, &tmp), (v3d_u8)l, src, blocks4((ULONG)w) * t->cpp,
                          x / 4, y / 4, (int)blocks4((ULONG)w), (int)blocks4((ULONG)h));
        return;
    }
    in = (ULONG)w * copy_bytes(class_of(t->f[0].format[0], t->f[0].type[0]));
    /* RGBA8 is stored as the RAM copy holds it: straight from there. */
    if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA8) {
        if (whole && store_tfu(t, k, l, src, (ULONG)w * 4, w, h))
            return;
        v3d_texture_store(ogles2_device(), face_gpu(t, k, &tmp), (v3d_u8)l, src, (ULONG)w * 4, x, y, w, h);
        return;
    }
    if (!(packed = AllocVec(row * h + 4, MEMF_ANY)))
        return;
    for (j = 0; j < h; j++)
        if (float_type(t->hw_type) || depth_type(t->hw_type))
            pack_wide(t, src + (ULONG)j * in, packed + (ULONG)j * row, (ULONG)w);
        else
            pack(t, src + (ULONG)j * in, packed + (ULONG)j * row, (ULONG)w);
    if (!whole || !store_tfu(t, k, l, packed, row, w, h))
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
    if (v3d_texture_alloc_blocks(dev, t->gpu, f0->w[0], f0->h[0], t->cpp,
                                 level_class(f0, 0) == CLASS_BLOCKS ? 4 : 1) < 0 ||
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
    {
        extern int g_v3d_uif_fast; /* v3dcore's direct UIF store, checked at its first use */
        static BOOL told;
        if (g_v3d_uif_fast == 2 && !told) {
            told = TRUE;
            ogles2_log("texture: v3dcore's direct UIF store differed from its general one: not used");
        }
    }
    /* A blank level is in the new copy already: its memory starts cleared
     * (SDL's testrendercopyex makes a render target a frame: 1.2 MB of
     * zeros stored at 640x480). */
    for (k = 0; k < faces(t); k++)
        for (l = 0; l < n; l++)
            if ((l == 0 || consistent(&t->f[k], l)) && !(t->f[k].blank & (1UL << l)))
                upload_level(t, k, l);
    return TRUE;
}

static BOOL face_target(GLenum target)
{
    return target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
}

/* The bound texture an image call's target names, and the face (*k);
 * 0 with GL_INVALID_ENUM set for another target. A rectangle texture
 * (ARB_texture_rectangle) has level 0 only: GL_INVALID_VALUE otherwise. */
static struct OGLES2Texture *image_texture(struct OGLES2Context *ctx, struct OGLES2TexState *ts, GLenum target,
                                           GLint level, int *k)
{
    *k = 0;
    if (face_target(target)) {
        *k = (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
        return ts->unitcube[ts->active];
    }
    if (target == GL_TEXTURE_2D)
        return ts->unit2d[ts->active];
    if (target == GL_TEXTURE_RECTANGLE) {
        if (level != 0) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return 0;
        }
        return ts->unitrect[ts->active];
    }
    ogles2_error(ctx, GL_INVALID_ENUM);
    return 0;
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
    /* A BGRA_EXT image is kept as RGBA8 (only its uploads swap), so it is
     * colour-renderable as RGBA is (EXT_texture_format_BGRA8888). */
    return (ty == GL_UNSIGNED_BYTE && (fm == GL_RGB || fm == GL_RGBA || fm == GL_BGRA_EXT)) ||
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

/* OES_depth_texture: level l of a 2D texture is a depth image; its depth
 * bits (16 or 24) and whether it holds stencil too (DEPTH_STENCIL_OES). */
BOOL ogles2_texture_depth(const struct OGLES2Texture *t, int l, UBYTE *bits, BOOL *stencil)
{
    const struct OGLES2Face *f = &t->f[0];
    int c;
    if (t->target != GL_TEXTURE_2D || l < 0 || l >= MAX_LEVELS || !(f->defined & (1UL << l)))
        return FALSE;
    c = level_class(f, l);
    if (c != CLASS_DEPTH16 && c != CLASS_DEPTH24)
        return FALSE;
    *bits = c == CLASS_DEPTH16 ? 16 : 24;
    *stencil = f->format[l] == GL_DEPTH_STENCIL_OES;
    return TRUE;
}

/* A depth texture's level as a job's depth (and stencil) image, its GPU
 * storage made first: the tile buffer stores into the level's own layout
 * as D16 or D24S8, which the TMU then samples as DEPTH_COMP16 or
 * DEPTH24_X8 (Mesa's depth textures). FALSE: none. */
BOOL ogles2_texture_zs_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int l,
                               struct OGLES2Surface *s)
{
    const V3DTexture *g;
    UBYTE bits;
    BOOL st;
    (void)ctx;
    if (!ogles2_texture_depth(t, l, &bits, &st) || !consistent(&t->f[0], l))
        return FALSE;
    if ((!t->gpu || l >= t->gpu->num_levels) && !upload_all(t))
        return FALSE;
    if (!(g = t->gpu) || l >= g->num_levels)
        return FALSE;
    s->addr = V3D_ALIGN_UP((v3d_uintptr)g->texture_mem.hostptr, 256) + g->levels[l].offset;
    s->bytes = g->levels[l].stride * g->levels[l].padded_h;
    s->mem_format = g->levels[l].tiling_format;
    s->format = bits == 16 ? V3D_OUTPUT_IMAGE_FORMAT_D16 : V3D_OUTPUT_IMAGE_FORMAT_D24S8;
    s->force_alpha1 = FALSE;
    s->ub = s->mem_format == V3D_MEMORY_FORMAT_UIF_NO_XOR || s->mem_format == V3D_MEMORY_FORMAT_UIF_XOR
                ? g->levels[l].padded_h / (2 * v3d_utile_height((int)t->cpp))
                : 0;
    return TRUE;
}

/* draw.c: unit `unit`'s texture of that kind returns 32 bits (by its level
 * 0's format; an incomplete one samples the RGBA8 fallback, which the
 * 32-bit variants read correctly too). */
BOOL ogles2_texture_wide(struct OGLES2Context *ctx, int unit, int kind)
{
    struct OGLES2TexState *ts = ctx->tex;
    const struct OGLES2Texture *t;
    if (!ts)
        return FALSE;
    t = kind == OGLES2_TEX_CUBE ? ts->unitcube[unit] : kind == OGLES2_TEX_RECT ? ts->unitrect[unit] : ts->unit2d[unit];
    return (t->f[0].defined & 1) && wide_class(t->f[0].format[0], t->f[0].type[0]);
}

void ogles2_texture_rendered(struct OGLES2Texture *t, int k, int l)
{
    t->f[k].rendered |= 1UL << l;
    t->f[k].blank &= ~(1UL << l);
}

BOOL ogles2_texture_read(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h,
                         UBYTE *rgba, ULONG stride)
{
    const struct OGLES2Face *f = &t->f[k];
    int j;
    if (level_class(f, l) != CLASS_RGBA8)
        return FALSE;
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
    t->f[0].blank = 1;
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

static void set_level(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, GLint level, UBYTE *img,
                      GLsizei width, GLsizei height, GLenum format, GLenum type, BOOL given);

static void tex_image(struct OGLES2Context *ctx, GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
                         const void *pixels)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    GLenum err;
    UBYTE *img;
    ULONG cb;
    int k;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE && !face_target(target)) {
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
    /* EXT_texture_format_BGRA8888 (revision 1.4 on): its sized format names
     * the same image as BGRA_EXT. */
    if ((GLenum)internalformat == GL_BGRA8_EXT && format == GL_BGRA_EXT)
        internalformat = GL_BGRA_EXT;
    /* The 8-bit sized formats (OES_rgb8_rgba8's names) for the image they
     * store, as OES_required_internalformat takes them: GLES 3 programs and
     * dEQP's clip_control cases pass them (GLES 2.0 alone: GL_INVALID_VALUE). */
    if (((GLenum)internalformat == GL_RGBA8_OES && format == GL_RGBA) ||
        ((GLenum)internalformat == GL_RGB8_OES && format == GL_RGB))
        if (type == GL_UNSIGNED_BYTE)
            internalformat = (GLint)format;
    if ((GLenum)internalformat != format) {
        ogles2_error(ctx, internalformat == GL_RGBA || internalformat == GL_RGB ||
                          internalformat == GL_LUMINANCE || internalformat == GL_ALPHA ||
                          internalformat == GL_LUMINANCE_ALPHA || internalformat == GL_DEPTH_COMPONENT ||
                          internalformat == GL_DEPTH_STENCIL_OES ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
        return;
    }
    /* OES_depth_texture: 2D textures only (cube maps would need
     * OES_depth_texture_cube_map). */
    if ((format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES) && target != GL_TEXTURE_2D) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!(t = image_texture(ctx, ts, target, level, &k)))
        return;
    before_change(ctx, ts, t);
    cb = copy_bytes(class_of(format, type));
    img = width && height ? AllocVec((ULONG)width * height * cb, MEMF_ANY | MEMF_CLEAR) : 0;
    if (width && height && !img) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (class_of(format, type) != CLASS_RGBA8)
        ogles2_wide_seen |= wide_class(format, type);
    if (img && pixels)
        convert(img, (ULONG)width * cb, pixels, format, type, width, height, ctx->unpack_alignment);
    set_level(ctx, t, k, level, img, width, height, format, type, pixels != 0);
}

/* Level `level` of face k becomes `img`, its new RAM copy (0: empty);
 * `given`: it holds the image's texels, else zeros. The caller has dealt
 * with the jobs that use the texture (before_change). */
static void set_level(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, GLint level, UBYTE *img,
                      GLsizei width, GLsizei height, GLenum format, GLenum type, BOOL given)
{
    struct OGLES2Face *f = &t->f[k];
    if (f->shadow[level])
        FreeVec(f->shadow[level]);
    f->shadow[level] = img;
    f->w[level] = (UWORD)width;
    f->h[level] = (UWORD)height;
    f->format[level] = format;
    f->type[level] = type;
    f->defined |= 1UL << level;
    if (img && given)
        f->blank &= ~(1UL << level);
    else
        f->blank |= 1UL << level;
    if (!ctx->v3d)
        return; /* a state-only context keeps the images only */
    /* No GPU texture yet, a new level 0, or a level the chain has no room
     * for: rebuilt at its first use (a draw, ogles2_texture_state; a
     * framebuffer, ogles2_texture_surface). A program sends level 0 and
     * then each mip level: rebuilding at each stored every level before it
     * again (Quake III's load, M6). No job uses the old copy (before_change). */
    if (!t->gpu || (level == 0 && (faces(t) == 1 || t->gpu->width != f->w[0])) ||
        level >= t->gpu->num_levels || t->gpu->width != t->f[0].w[0] || t->gpu->height != t->f[0].h[0]) {
        free_gpu(t);
    } else if (consistent(f, level) && img) {
        upload_level(t, k, level);
    }
}

/* Timed for probe P (frame.c). */
void ogles2_glTexImage2D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
                         const void *pixels)
{
    ULONG t0 = ogles2_clock_us();
    tex_image(ctx, target, level, internalformat, width, height, border, format, type, pixels);
    if (ogles2_context_valid(ctx))
        ctx->sum_teximage += (double)(ULONG)(ogles2_clock_us() - t0);
}

static void tex_sub_image(struct OGLES2Context *ctx, GLenum target, GLint level, GLint xoffset,
                            GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
                            const void *pixels)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    struct OGLES2Face *f;
    GLenum err;
    UBYTE *sub;
    ULONG cb;
    int y, k;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!pixel_size(format, type, &err)) {
        ogles2_error(ctx, err);
        return;
    }
    if (!(t = image_texture(ctx, ts, target, level, &k)))
        return;
    f = &t->f[k];
    if (level < 0 || level >= MAX_LEVELS || !(f->defined & (1UL << level))) {
        ogles2_error(ctx, level < 0 || level >= MAX_LEVELS ? GL_INVALID_VALUE : GL_INVALID_OPERATION);
        return;
    }
    /* GLES 2.0 3.7.2: the level's own format; and the RAM copy's class
     * (float data into a float level, depth into a depth level). */
    if (format != f->format[level] || class_of(format, type) != level_class(f, level)) {
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
    cb = copy_bytes(level_class(f, level));
    if (!(sub = AllocVec((ULONG)width * height * cb, MEMF_ANY))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    convert(sub, (ULONG)width * cb, pixels, format, type, width, height, ctx->unpack_alignment);
    for (y = 0; y < height; y++)
        memcpy(f->shadow[level] + ((ULONG)(yoffset + y) * f->w[level] + xoffset) * cb,
               sub + (ULONG)y * width * cb, (ULONG)width * cb);
    f->blank &= ~(1UL << level);
    if (t->gpu && level < t->gpu->num_levels && consistent(f, level))
        store_box(t, k, level, sub, xoffset, yoffset, width, height);
    else if (!t->gpu && ctx->v3d && !upload_all(t)) /* renamed: a new copy, whole */
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
    FreeVec(sub);
}

/* Timed for probe P (frame.c). */
void ogles2_glTexSubImage2D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint xoffset,
                            GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
                            const void *pixels)
{
    ULONG t0 = ogles2_clock_us();
    tex_sub_image(ctx, target, level, xoffset, yoffset, width, height, format, type, pixels);
    if (ogles2_context_valid(ctx))
        ctx->sum_teximage += (double)(ULONG)(ogles2_clock_us() - t0);
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
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE && !face_target(target)) {
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
    if (!ogles2_fb_ready(ctx)) /* GL_INVALID_FRAMEBUFFER_OPERATION */
        return;
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
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ogles2_fb_ready(ctx)) /* GL_INVALID_FRAMEBUFFER_OPERATION */
        return;
    /* In the level's own format, so the conversion is table 3.15's (a
     * LUMINANCE texture takes red); glTexSubImage2D reports a level that
     * does not exist. */
    {
        struct OGLES2TexState *ts = ts_get(ctx);
        struct OGLES2Texture *t;
        int k;
        if (!ts || !(t = image_texture(ctx, ts, target, level, &k)))
            return;
        fmt = level >= 0 && level < MAX_LEVELS ? t->f[k].format[level] : 0;
        if (!base_components(fmt))
            fmt = GL_RGBA; /* an undefined level: glTexSubImage2D refuses it */
    }
    if (!width || !height) { /* nothing to copy; the level and offsets are still checked */
        ogles2_glTexSubImage2D(ctx, target, level, xoffset, yoffset, width, height, fmt,
                               GL_UNSIGNED_BYTE, 0);
        return;
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

/* ---- compressed textures ----
 * OES_compressed_ETC1_RGB8_texture: decoded into the RGBA8 storage (etc1.c)
 * and stored as glTexImage2D does; the level then keeps the ETC1 format,
 * which no TexSubImage may change.
 * S3TC (DXT1, DXT3, DXT5; M7 step 3): kept as given, in RAM and on the GPU,
 * which samples BC1-BC3 itself. glCompressedTexSubImage2D replaces whole
 * blocks; glTexSubImage2D, glCopyTexSubImage2D (another format: refused by
 * glTexSubImage2D) and glGenerateMipmap give GL_INVALID_OPERATION, and a
 * framebuffer object cannot draw into one (not colour-renderable). */
#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif

/* Level `level` of an S3TC texture: `bytes` of blocks (data 0: zeros). */
static void dxt_image(struct OGLES2Context *ctx, struct OGLES2TexState *ts, GLenum target, GLint level,
                      GLenum format, GLsizei width, GLsizei height, ULONG bytes, const void *data)
{
    struct OGLES2Texture *t;
    UBYTE *img;
    int k;
    if (width > (MAX_SIZE >> level) || height > (MAX_SIZE >> level) || (face_target(target) && width != height)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(t = image_texture(ctx, ts, target, level, &k)))
        return;
    before_change(ctx, ts, t);
    img = bytes ? AllocVec(bytes, MEMF_ANY | MEMF_CLEAR) : 0;
    if (bytes && !img) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (img && data)
        CopyMem((APTR)data, img, bytes);
    set_level(ctx, t, k, level, img, width, height, format, 0, data != 0);
}

void ogles2_glCompressedTexImage2D(struct OGLES2Context *ctx, GLenum target, GLint level,
                                   GLenum internalformat, GLsizei width, GLsizei height, GLint border,
                                   GLsizei imageSize, const void *data)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    ULONG bw, bh, bs;
    UBYTE *img, unpack;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    bs = dxt_block_bytes(internalformat);
    if (internalformat != GL_ETC1_RGB8_OES && !bs) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    bw = blocks4((ULONG)width);
    bh = blocks4((ULONG)height);
    if (width < 0 || height < 0 || border != 0 || level < 0 || level >= MAX_LEVELS ||
        imageSize != (GLsizei)(bw * bh * (bs ? bs : 8))) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (bs) {
        dxt_image(ctx, ts, target, level, internalformat, width, height, (ULONG)imageSize, data);
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
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    struct OGLES2Face *f;
    ULONG bs, bw, bh, row, j;
    int k;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    bs = dxt_block_bytes(format);
    if ((target != GL_TEXTURE_2D && !face_target(target)) || (format != GL_ETC1_RGB8_OES && !bs)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* The order of the checks is dEQP's (negative_api.texture): values
     * first, then the level, its bounds and size; the format and the block
     * bounds last. */
    if (level < 0 || level >= MAX_LEVELS || xoffset < 0 || yoffset < 0 || width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(t = image_texture(ctx, ts, target, level, &k)))
        return;
    f = &t->f[k];
    if (!(f->defined & (1UL << level))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    bw = blocks4((ULONG)width);
    bh = blocks4((ULONG)height);
    if (xoffset + width > f->w[level] || yoffset + height > f->h[level] ||
        imageSize != (GLsizei)(bw * bh * (bs ? bs : 8))) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* OES_compressed_ETC1_RGB8_texture: ETC1 images cannot be updated; S3TC
     * (EXT_texture_compression_s3tc): the level's own format, whole blocks
     * (a box may end at the level's edge). */
    if (!bs || format != f->format[level] || ((xoffset | yoffset) & 3) ||
        ((width & 3) && xoffset + width != f->w[level]) || ((height & 3) && yoffset + height != f->h[level])) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!width || !height || !data)
        return;
    before_change(ctx, ts, t);
    row = blocks4(f->w[level]) * bs;
    for (j = 0; j < bh; j++)
        CopyMem((UBYTE *)data + j * bw * bs, f->shadow[level] + ((ULONG)yoffset / 4 + j) * row + (ULONG)xoffset / 4 * bs,
                bw * bs);
    f->blank &= ~(1UL << level);
    if (t->gpu && level < t->gpu->num_levels && consistent(f, level))
        store_box(t, k, level, (const UBYTE *)data, xoffset, yoffset, width, height);
    else if (!t->gpu && ctx->v3d && !upload_all(t)) /* renamed: a new copy, whole */
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
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

/* The same for an RGBA float copy. */
static void box_down_float(const GLfloat *src, int sw, int sh, GLfloat *dst, int dw, int dh)
{
    int x, y, c;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            int x0 = x * 2, y0 = y * 2;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
            for (c = 0; c < 4; c++)
                dst[(y * dw + x) * 4 + c] = (src[(y0 * sw + x0) * 4 + c] + src[(y0 * sw + x1) * 4 + c] +
                                             src[(y1 * sw + x0) * 4 + c] + src[(y1 * sw + x1) * 4 + c]) * 0.25f;
        }
}

/* glGenerateMipmap on the GPU (plan 7.6's "later"): the TFU filters levels
 * 1 .. n-1 from level 0 where it is, in the texture's GPU memory. The CPU
 * path below filters the RAM copies and uploads the whole chain, after a
 * read-back when the GPU drew level 0: glmark2's refract, a 1600x1200
 * render target every frame, spent 500 ms a frame there. Only for a 2D
 * texture whose GPU copy already has the chain, laid out by an earlier
 * glGenerateMipmap or the levels given (the first call takes the CPU path),
 * in a format the TFU filters. The levels it writes are newer on the GPU
 * than their RAM copies, as a render target's are (read back when the CPU
 * needs them, sync). Probe M keeps the CPU path. FALSE: the CPU path runs. */
static BOOL generate_tfu(struct OGLES2Context *ctx, struct OGLES2TexState *ts, struct OGLES2Texture *t, int n)
{
    struct OGLES2Face *f = &t->f[0];
    struct OGLES2Context *m, *user;
    ULONG made = ((1UL << n) - 1) & ~1UL; /* levels 1 .. n-1 */
    int l, r;
    if ((ctx->probe & OGLES2_PROBE_CPU_MIPMAP) || !ctx->v3d || faces(t) != 1 || n < 2 || !t->gpu ||
        !t->gpu->texture_mem.hostptr || t->gpu->num_levels < n || !v3d_tfu_supports_mipmaps(t->hw_type))
        return FALSE;
    for (l = 1; l < n; l++) {
        int w = f->w[l - 1] > 1 ? f->w[l - 1] / 2 : 1, h = f->h[l - 1] > 1 ? f->h[l - 1] / 2 : 1;
        if (!(f->defined & (1UL << l)) || !f->shadow[l] || f->w[l] != w || f->h[l] != h ||
            f->format[l] != f->format[0] || f->type[l] != f->type[0])
            return FALSE;
    }
    /* The job that draws level 0 rendered first (any member's)... */
    for (m = ctx->share->first; m; m = m->share_next)
        ogles2_fbo_texture_changing(m, t);
    /* ...and one that samples the texture and has not rendered yet: it must
     * see the old levels, which the TFU writes over in place (renaming, as
     * before_change does, would need the levels in RAM: the read-back this
     * path is for). Usually rendered already: the next job's start waits
     * for the one before. */
    if (t->used_serial && (user = ogles2_job_user(ctx, t->used_serial)) != 0 && user->v3d) {
        ogles2_job_flush(user, FALSE);
        ogles2_job_finish(user);
    }
    CacheClearE(0, ~0UL, CACRF_ClearD); /* the CPU's writes to level 0 in memory */
    if (!v3d_gpu_lock(ogles2_device_gpu_lock(), TFU_LOCK_TIMEOUT_MS))
        return FALSE;
    r = v3d_tfu_generate_mipmaps(ogles2_device(), t->gpu, t->hw_type, (v3d_u8)(n - 1));
    v3d_gpu_unlock(ogles2_device_gpu_lock());
    if (r) {
        if (r == -2)
            ogles2_log("glGenerateMipmap: the TFU did not finish (texture %ld, %ldx%ld): "
                       "the CPU makes the levels", (ULONG)t->name, (ULONG)f->w[0], (ULONG)f->h[0]);
        return FALSE;
    }
    f->rendered |= made;
    f->blank &= ~made;
    if (!ts->logged_tfu) {
        ts->logged_tfu = TRUE;
        ogles2_log("glGenerateMipmap: the TFU (texture %ld, %ldx%ld, %ld levels; logged once)",
                   (ULONG)t->name, (ULONG)f->w[0], (ULONG)f->h[0], (ULONG)n);
    }
    return TRUE;
}

void ogles2_glGenerateMipmap(struct OGLES2Context *ctx, GLenum target)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    int l, n, k, cls;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target == GL_TEXTURE_RECTANGLE) { /* ARB_texture_rectangle: no mip levels */
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(t = bound(ctx, ts, target)))
        return;
    /* A compressed level 0 (ETC1, S3TC), of any size: GLES 2.0 3.7.11. */
    if ((t->f[0].defined & 1) && (t->f[0].format[0] == GL_ETC1_RGB8_OES || dxt_block_bytes(t->f[0].format[0]))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!base_ok(t)) {
        /* GLES 2.0 3.7.11: a cube map that is not cube complete. */
        if (target == GL_TEXTURE_CUBE_MAP)
            ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* Depth textures: no mip chain to filter (GLES 3.0's rule for formats
     * that are not filterable; OES_depth_texture says nothing). */
    cls = level_class(&t->f[0], 0);
    if (cls == CLASS_DEPTH16 || cls == CLASS_DEPTH24) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    n = level_count(t->f[0].w[0], t->f[0].h[0]);
    if (generate_tfu(ctx, ts, t, n))
        return;
    before_change(ctx, ts, t);
    for (k = 0; k < faces(t); k++) {
        struct OGLES2Face *f = &t->f[k];
        for (l = 1; l < n; l++) {
            int w = f->w[l - 1] > 1 ? f->w[l - 1] / 2 : 1, h = f->h[l - 1] > 1 ? f->h[l - 1] / 2 : 1;
            UBYTE *img = AllocVec((ULONG)w * h * copy_bytes(cls), MEMF_ANY);
            if (!img) {
                ogles2_error(ctx, GL_OUT_OF_MEMORY);
                return;
            }
            if (cls == CLASS_FLOAT)
                box_down_float((const GLfloat *)f->shadow[l - 1], f->w[l - 1], f->h[l - 1], (GLfloat *)img, w, h);
            else
                box_down(f->shadow[l - 1], f->w[l - 1], f->h[l - 1], img, w, h);
            if (f->shadow[l])
                FreeVec(f->shadow[l]);
            f->shadow[l] = img;
            f->w[l] = (UWORD)w;
            f->h[l] = (UWORD)h;
            f->format[l] = f->format[l - 1];
            f->type[l] = f->type[l - 1];
            f->defined |= 1UL << l;
            f->blank &= ~(1UL << l);
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

static UBYTE wrap_mode(GLenum w)
{
    return w == GL_CLAMP_TO_EDGE ? V3D_WRAP_MODE_CLAMP
         : w == GL_MIRRORED_REPEAT ? V3D_WRAP_MODE_MIRROR
         : w == GL_MIRROR_CLAMP_TO_EDGE ? V3D_WRAP_MODE_MIRRORONCE : V3D_WRAP_MODE_REPEAT;
}

/* The texture and sampler state for unit `unit`'s texture of `cube`
 * kind, written into the job's state buffer (packing as PiStorm3D's
 * v3d_texture_emit_state; values as Mesa's v3dx_state.c). FALSE: out of
 * memory. */
BOOL ogles2_texture_state(struct OGLES2Context *ctx, int unit, int kind, ULONG *tex_state,
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
    int first = 0, last = 0;
    BOOL own;

    if (!ts)
        return FALSE;
    t = kind == OGLES2_TEX_CUBE ? ts->unitcube[unit] : kind == OGLES2_TEX_RECT ? ts->unitrect[unit] : ts->unit2d[unit];
    /* The same records as the last draw with it: a texture a draw of a job
     * not yet rendered used gets a new GPU copy (another address) when its
     * images change (before_change); its parameters are compared here.
     * Records in the texture's own block hold across jobs (Quake III: most
     * of a frame's textures are drawn once a frame, 2.5 us a record). */
    if (t->gpu && (t->st_own || (t->st_gen == g_v3d_cl_generation && t->st_ctx == ctx)) &&
        t->st_mem == t->gpu->texture_mem.hostptr && t->st_pver == t->pver) {
        t->used_serial = ctx->job_serial;
        *tex_state = t->st_tex;
        *sampler_state = t->st_smp;
        return TRUE;
    }
    /* A renamed texture gets its new copy at its first draw at the latest. */
    if (!t->gpu && t != &ts->fallback)
        upload_all(t);
    if (!complete(t, &first, &last)) {
        if (!t->warned_incomplete && t != &ts->fallback) {
            int l, n = mipmapped(t->min_filter) && t->f[0].w[0] ? level_count(t->f[0].w[0], t->f[0].h[0]) : 1;
            t->warned_incomplete = TRUE;
            for (l = 1; l < n && consistent(&t->f[0], l); l++)
                ;
            ogles2_log("texture %ld sampled while incomplete (it reads black): %s, level 0 %ldx%ld, "
                       "min filter 0x%04lx, level %ld of %ld missing or inconsistent (logged once a texture)",
                       (ULONG)t->name, (ULONG)(!t->gpu ? "no GPU copy" : base_ok(t) ? "mip chain" : "level 0"),
                       (ULONG)t->f[0].w[0], (ULONG)t->f[0].h[0], (ULONG)t->min_filter, (ULONG)l, (ULONG)n);
        }
        if (!(t = fallback(ts)))
            return FALSE;
        first = last = 0;
    }
    /* Into the texture's own block, unless a job not yet rendered may read
     * the records it holds (the last job that drew with the texture): then
     * into this job's state buffer, as before. */
    own = !t->used_serial || !ogles2_job_uses(ctx, t->used_serial);
    if (own && !t->rec.hostptr && v3d_mem_alloc(ogles2_device(), &t->rec, 64) < 0)
        own = FALSE;
    t->used_serial = ctx->job_serial;
    g = t->gpu;

    if (own)
        tsr = (v3d_texture_shader_state *)t->rec.hostptr;
    else if (!(tsr = claim32(v, sizeof(*tsr))))
        return FALSE;
    memset(tsr, 0, sizeof(*tsr));
    tiled = (char *)V3D_ALIGN_UP((v3d_uintptr)g->texture_mem.hostptr, 256);
    tsr->texture_base_pointer_rshift_6 = ((ULONG)tiled + g->levels[0].offset) >> 6;
    tsr->flip_texture_y_axis = FALSE;
    tsr->base_level = (ULONG)first; /* SGIS_texture_lod */
    tsr->max_level = (ULONG)last;
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

    if (own)
        ss = (v3d_sampler_state *)((UBYTE *)t->rec.hostptr + 32);
    else if (!(ss = claim32(v, sizeof(*ss))))
        return FALSE;
    memset(ss, 0, sizeof(*ss));
    ss->mag_filter_nearest = t->mag_filter == GL_NEAREST;
    ss->min_filter_nearest = t->min_filter == GL_NEAREST || t->min_filter == GL_NEAREST_MIPMAP_NEAREST ||
                             t->min_filter == GL_NEAREST_MIPMAP_LINEAR;
    ss->mip_filter_nearest = t->min_filter != GL_NEAREST_MIPMAP_LINEAR &&
                             t->min_filter != GL_LINEAR_MIPMAP_LINEAR;
    /* 32-bit floats and depth: the TMU does not filter them. */
    if (unfilterable_type(t->hw_type) &&
        !(ss->mag_filter_nearest && ss->min_filter_nearest && ss->mip_filter_nearest)) {
        ss->mag_filter_nearest = ss->min_filter_nearest = ss->mip_filter_nearest = TRUE;
        if (!t->warned_filter) {
            t->warned_filter = TRUE;
            ogles2_log("texture %ld: 32-bit float or depth texels are sampled nearest (no linear filtering "
                       "for them; logged once a texture)", (ULONG)t->name);
        }
    }
    ss->depth_compare_function = V3D_COMPARE_FUNC_NEVER;
    ss->wrap_s = wrap_mode(t->wrap_s);
    ss->wrap_t = wrap_mode(t->wrap_t);
    /* The LOD range, Mesa's v3dx_state.c: SGIS_texture_lod's limits within
     * the field's 0 to 15; without mipmapping clamped just above the base
     * level, so the hardware still chooses between the min and mag filters. */
    {
        GLfloat mn = t->min_lod < 0.0f ? 0.0f : t->min_lod > 15.0f ? 15.0f : t->min_lod;
        GLfloat mx = t->max_lod < mn ? mn : t->max_lod > 15.0f ? 15.0f : t->max_lod;
        GLfloat bias = t->lod_bias < -MAX_LOD_BIAS ? -MAX_LOD_BIAS
                     : t->lod_bias > MAX_LOD_BIAS ? MAX_LOD_BIAS : t->lod_bias;
        if (!mipmapped(t->min_filter)) {
            if (mn > 1.0f / 256.0f) mn = 1.0f / 256.0f;
            if (mx > 1.0f / 256.0f) mx = 1.0f / 256.0f;
        }
        ss->min_level_of_detail = V3D_FLOAT_TO_U4_8(mn);
        ss->max_level_of_detail = V3D_FLOAT_TO_U4_8(mx);
        /* EXT_texture_lod_bias: s8.8 */
        ss->fixed_bias = (UWORD)(WORD)round_param(bias * 256.0f);
    }
    /* EXT_texture_filter_anisotropic, Mesa's levels: 2, 4, 8, 16 */
    if (t->max_aniso > 1.0f && !unfilterable_type(t->hw_type)) {
        ss->anisotropy_enable = TRUE;
        ss->maximum_anisotropy = t->max_aniso > 8.0f ? 3 : t->max_aniso > 4.0f ? 2 : t->max_aniso > 2.0f ? 1 : 0;
    }
    swivel24 = (UBYTE *)ss;
    temp = swivel24[3];
    swivel24[3] = swivel24[1];
    swivel24[1] = temp;
    swivel16 = (UWORD *)ss;
    swivel16[2] = LE16(swivel16[2]); /* the bias */
    swivel16[3] = LE16(swivel16[3]);
    *sampler_state = (ULONG)ss;
    t->st_gen = g_v3d_cl_generation;
    t->st_ctx = ctx;
    t->st_own = own;
    t->st_mem = g->texture_mem.hostptr;
    t->st_pver = t->pver;
    t->st_tex = *tex_state;
    t->st_smp = *sampler_state;
    return TRUE;
}
