/*
 * The draw path (plan section 7.9): client-side vertex arrays and
 * glDrawArrays with the linked program's image: a .v3dp program binary's
 * compiled variants (M2), or the built-in image for clears.
 *
 * Per draw, into the job's state buffer: the uniform streams of the binning,
 * vertex and fragment shaders (their recipes, plan 8.6), a copy of each
 * enabled array's [first, first+count) byte-swapped for the GPU, a 16-byte
 * constant for each disabled one, then the GL shader state record and one
 * attribute record per vertex input slot, filled from the variants'
 * prog_data as Mesa's v3dx_draw.c does. Into the binning list: the state
 * packets that changed (clip window, CFG_BITS, viewport, blend, colour mask;
 * deduplicated per control list as Mesa and PiStorm3D do), the per-pass
 * residue on a pass's first draw, VCM_CACHE_SIZE, GL_SHADER_STATE and
 * VERTEX_ARRAY_PRIMS, in Mesa's order (v3dx_emit.c, v3dx_draw.c).
 *
 * Every claim is filled before the next one is made: a claim that grows the
 * state buffer flushes the old block and starts a new one (v3d_clbuf.c), so
 * a filled block's addresses stay valid until the frame retires.
 *
 * Licence: this file adapts PiStorm3D's code, so it is under the Hyperion
 * MiniGL Open Source License (LICENSE, section 1).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_commands.h"
#include "v3d_clbuf.h"
#include "v3d_hw.h"

extern v3d_u32 g_v3d_cl_generation; /* v3d_frame.c: bumped whenever a control list restarts */

/* Last packet values emitted into the current binning list (Mesa's dirty
 * flags, PiStorm3D's mglv3d_dedup_cache). Valid while gen matches. */
struct dedup
{
    v3d_u32 gen;
    /* What the last emit_state was made from: the same again skips it. */
    ULONG ver;
    const struct OGLES2Framebuffer *fb;
    UWORD fb_w, fb_h;
    UBYTE fb_depth_bits, fb_stencil_bits;
    const struct OGLES2Variant *fs;
    BOOL cfg, cw, vp, blend, cwm, lw, st;
    UBYTE effp, erfp, cp, edo, dtf, zue, eze, ezue, be, se, wf, pf, pv, rom;
    UBYTE smask;          /* the SAMPLE_STATE mask last sent (4x MSAA) */
    UBYTE stencil[2][5];  /* the STENCIL_CFG bytes last sent, front and back */
    UWORD cw_l, cw_t, cw_w, cw_h;
    UWORD vp_w, vp_h, vp_cx2, vp_cy2;
    UBYTE b_src, b_dst, b_eq, b_asrc, b_adst, b_aeq;
    UWORD b_color[4];
    ULONG cwm_mask;
    float lw_value, do_factor, do_units;
    float vp_zs, vp_zo;
    BOOL vp_down;
    BOOL do_valid;
    int prim;
};

struct OGLES2Draw
{
    v3d_mem builtin_code;          /* the built-in image's code, uploaded at first use */
    struct OGLES2Program quad;     /* clears drawn as rectangles (built-in image) */
    BOOL quad_ready;
    /* and into several render targets (EXT_draw_buffers): clear_mrt_v3dp.h */
    struct OGLES2Program mrt_quad;
    struct OGLES2Image *mrt_image;
    BOOL mrt_ready, mrt_failed;
    /* and glCopyTex* on the GPU (copy.c): copy_tex_v3dp.h */
    struct OGLES2Program copy_prog;
    struct OGLES2Image *copy_image;
    BOOL copy_ready, copy_failed;
    struct dedup dd;
    v3d_u32 defattr_gen;           /* the default attribute block of this control list */
    ULONG defattr;
    const struct OGLES2Image *missed; /* the last image a variant miss was logged for */
    const struct OGLES2Image *missed_rt; /* and a render-target miss (EXT_draw_buffers) */
    v3d_mem spill;                 /* register spilling's scratch area (spill_area) */
    ULONG spill_size;              /* bytes a thread has in it */
};

/* ---- vertex arrays ---- */

static int type_size(GLenum type)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:   return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_FLOAT: case GL_FIXED:          return 4;
    default:                               return 0;
    }
}

void ogles2_glVertexAttribPointer(struct OGLES2Context *ctx, GLuint index, GLint size, GLenum type,
                                  GLboolean normalized, GLsizei stride, const void *pointer)
{
    struct OGLES2Array *a;
    if (!ogles2_context_valid(ctx))
        return;
    if (index >= OGLES2_MAX_VERTEX_ATTRIBS || size < 1 || size > 4 || stride < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!type_size(type)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    a = &ctx->array[index];
    a->size = size;
    a->type = type;
    a->normalized = normalized ? GL_TRUE : GL_FALSE;
    a->stride = stride;
    a->pointer = pointer;
    a->buffer = ogles2_buffer_array(ctx); /* GLES 2.0 2.9: captured now */
}

static void enable_array(struct OGLES2Context *ctx, GLuint index, GLboolean on)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (index >= OGLES2_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    ctx->array[index].enabled = on;
}

void ogles2_glEnableVertexAttribArray(struct OGLES2Context *ctx, GLuint index)
{
    enable_array(ctx, index, GL_TRUE);
}

void ogles2_glDisableVertexAttribArray(struct OGLES2Context *ctx, GLuint index)
{
    enable_array(ctx, index, GL_FALSE);
}

void ogles2_glGetVertexAttribiv(struct OGLES2Context *ctx, GLuint index, GLenum pname,
                                GLint *params)
{
    const struct OGLES2Array *a;
    int k;
    if (!ogles2_context_valid(ctx) || !params)
        return;
    if (index >= OGLES2_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    a = &ctx->array[index];
    switch (pname) {
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED:        *params = a->enabled; break;
    case GL_VERTEX_ATTRIB_ARRAY_SIZE:           *params = a->size; break;
    case GL_VERTEX_ATTRIB_ARRAY_STRIDE:         *params = a->stride; break;
    case GL_VERTEX_ATTRIB_ARRAY_TYPE:           *params = (GLint)a->type; break;
    case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:     *params = a->normalized; break;
    case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: *params = (GLint)ogles2_buffer_name(a->buffer); break;
    case GL_CURRENT_VERTEX_ATTRIB:
        for (k = 0; k < 4; k++)
            params[k] = (GLint)ctx->state.attrib[index][k];
        break;
    default:
        ogles2_error(ctx, GL_INVALID_ENUM);
        break;
    }
}

void ogles2_glGetVertexAttribPointerv(struct OGLES2Context *ctx, GLuint index, GLenum pname,
                                      void **pointer)
{
    if (!ogles2_context_valid(ctx) || !pointer)
        return;
    if (index >= OGLES2_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    *pointer = (void *)ctx->array[index].pointer;
}

/* ---- per-context draw state ---- */

static struct OGLES2Draw *draw_state(struct OGLES2Context *ctx)
{
    struct OGLES2Draw *d = ctx->draw;
    if (!d) {
        if (!(d = AllocVec(sizeof(*d), MEMF_ANY | MEMF_CLEAR)))
            return 0;
        d->dd.gen = ~0UL;
        ctx->draw = d;
    }
    return d;
}

/* The GPU address of a variant's code. A loaded image carries its own; the
 * built-in one is uploaded once per context (it is static data). */
static ULONG code_address(struct OGLES2Draw *d, const struct OGLES2Image *im,
                          const struct OGLES2Variant *v)
{
    if (im->code_mem)
        return (ULONG)im->code_mem->hostptr + v->code_offset;
    if (im != ogles2_builtin_image())
        return 0; /* loaded without a GPU: never drawn */
    if (!d->builtin_code.hostptr) {
        ULONG len;
        UWORD i;
        if (v3d_mem_alloc(ogles2_device(), &d->builtin_code, im->code_total) < 0) {
            d->builtin_code.hostptr = 0;
            return 0;
        }
        for (i = 0; i < im->nvariants; i++)
            memcpy((UBYTE *)d->builtin_code.hostptr + im->variants[i].code_offset,
                   im->variants[i].code, im->variants[i].code_bytes);
        len = d->builtin_code.size;
        CachePreDMA(d->builtin_code.hostptr, &len, 0);
    }
    return (ULONG)d->builtin_code.hostptr + v->code_offset;
}

void ogles2_draw_free(struct OGLES2Context *ctx)
{
    struct OGLES2Draw *d = ctx->draw;
    if (!d)
        return;
    if (d->builtin_code.hostptr)
        v3d_mem_free(ogles2_device(), &d->builtin_code);
    if (d->spill.hostptr)
        v3d_mem_free(ogles2_device(), &d->spill);
    if (d->quad_ready)
        ogles2_program_builtin_free(&d->quad);
    if (d->mrt_ready)
        ogles2_program_builtin_free(&d->mrt_quad);
    if (d->mrt_image)
        ogles2_image_free(d->mrt_image, ctx);
    if (d->copy_ready)
        ogles2_program_builtin_free(&d->copy_prog);
    if (d->copy_image)
        ogles2_image_free(d->copy_image, ctx);
    FreeVec(d);
    ctx->draw = 0;
}

/* ---- state buffer claims ---- */

/* `size` bytes of the job's state buffer at `align` (a power of two; the
 * blocks themselves are 64-byte aligned). NULL when memory ran out. */
static void *claim(V3DContext *v, ULONG size, ULONG align)
{
    v3d_u8 s = v->build_slot;
    v3d_static_buffer *sb = &v->state_buf[s];
    sb->used = (sb->used + (int)align - 1) & ~((int)align - 1);
    return v3d_cl_claim_fast(ogles2_device(), &v->state_mem[s], sb, size, &v->frame);
}

static ULONG le32(ULONG x)
{
    return (x >> 24) | ((x >> 8) & 0xff00UL) | ((x << 8) & 0xff0000UL) | (x << 24);
}

static ULONG float_bits(GLfloat f)
{
    union { GLfloat f; ULONG u; } c;
    c.f = f;
    return c.u;
}

/* EXT_clip_control's ZERO_TO_ONE (docs/M7_EXTENSIONS_PLAN.md step 4). V3D
 * 4.2 clips clip-space z to -w..w only: its window-space Z planes clamp
 * (the m7-3 probe). A program compiled with Mesa patch 0013 turns z into
 * 2z - w itself (OGLES2_U_CLIP_Z_COEFF -1), which the hardware clips and
 * maps as -w..w: the viewport stays GL's. An older binary gets the 0..w
 * mapping from the viewport instead: its depth is right, but what lies
 * before the near plane is drawn at depth n. */
static BOOL zero_to_one_in_viewport(const struct OGLES2Context *ctx)
{
    return ctx->state.clip_depth == GL_ZERO_TO_ONE_EXT &&
           !(ctx->program && ctx->program->image && ctx->program->image->clip_z_coeff);
}

/* The viewport's Z scale and offset: clip-space z from -w..w to the depth
 * range, or from 0..w (Mesa's _mesa_get_viewport_xform). */
static void viewport_z(const struct OGLES2Context *ctx, float *zs, float *zo)
{
    const struct OGLES2State *s = &ctx->state;
    if (zero_to_one_in_viewport(ctx)) {
        *zs = s->depth_range[1] - s->depth_range[0];
        *zo = s->depth_range[0];
    } else {
        *zs = (s->depth_range[1] - s->depth_range[0]) * 0.5f;
        *zo = (s->depth_range[1] + s->depth_range[0]) * 0.5f;
    }
}

/* The picture is drawn top-down in the framebuffer's rows: the default
 * framebuffer (plan 7.11), unless EXT_clip_control's upper-left origin turns
 * it over again; a framebuffer object only with that origin. A negative Y
 * scale, and the winding of front faces reversed (Mesa's state tracker). */
static BOOL y_down(const struct OGLES2Context *ctx)
{
    return ctx->fb_flip ^ (ctx->state.clip_origin == GL_UPPER_LEFT_EXT);
}

/* The program's state variables (FORMAT.md STAGE_PARAMS kind 2), computed
 * into its constant buffers before a draw, as Mesa's prog_statevars.c does
 * for a framebuffer drawn upside down (FlipY: the default framebuffer). */
static void update_state_vars(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    const struct OGLES2Image *im = p->image;
    const GLfloat *dr = ctx->state.depth_range;
    UWORD i;
    for (i = 0; i < im->nstatevars; i++) {
        const struct OGLES2StateVar *sv = &im->statevars[i];
        GLfloat v[4];
        int k;
        switch (sv->state) {
        case OGLES2_STATE_DEPTH_RANGE:
            v[0] = dr[0];
            v[1] = dr[1];
            v[2] = dr[1] - dr[0];
            v[3] = 1.0f;
            break;
        case OGLES2_STATE_FB_WPOS_Y_TRANSFORM:
            /* Mesa's prog_statevars.c: flipped (-1, height, 1, 0), else
             * (1, 0, -1, height) */
            v[0] = ctx->fb_flip ? -1.0f : 1.0f;
            v[1] = ctx->fb_flip ? (GLfloat)ctx->fb_h : 0.0f;
            v[2] = ctx->fb_flip ? 1.0f : -1.0f;
            v[3] = ctx->fb_flip ? 0.0f : (GLfloat)ctx->fb_h;
            break;
        case OGLES2_STATE_FB_PNTC_Y_TRANSFORM:
            /* upper-left sprite origin: inverted on a flipped buffer only */
            v[0] = ctx->fb_flip ? -1.0f : 1.0f;
            v[1] = ctx->fb_flip ? 1.0f : 0.0f;
            v[2] = 0.0f;
            v[3] = 0.0f;
            break;
        default:
            continue;
        }
        for (k = 0; k < 4; k++)
            if (p->cbuf[sv->stage][sv->offset + k] != float_bits(v[k]))
                ogles2_cbuf_set(p, sv->stage, sv->offset + k, float_bits(v[k]));
    }
}

/* Texture and sampler state per texture index of each stage (the TMU
 * recipes' unit field), [0] vertex, [1] fragment; 0 where unused. */
struct tex_states
{
    ULONG tex[2][OGLES2_MAX_STAGE_SAMPLERS], smp[2][OGLES2_MAX_STAGE_SAMPLERS];
    ULONG ubo[2];   /* little-endian copy of each stage's constant buffer, if read */
};

/* QUNIFORM_UBO_ADDR (indirectly indexed uniform arrays, plan 7.8): the
 * shader reads its constant buffer through the TMU, so a stage whose recipe
 * asks for it gets a little-endian copy in the state buffer. Made before any
 * uniform stream (a claim must not happen while another is half filled). */
static BOOL ubo_copies(struct OGLES2Context *ctx, const struct OGLES2Program *p,
                       const struct OGLES2Variant *const v[3], struct tex_states *ts)
{
    int k, si;
    for (k = 0; k < 3; k++) {
        si = v[k]->stage == OGLES2_VARIANT_FS ? 1 : 0;
        if (ts->ubo[si])
            continue;
        if (v[k]->uses_ubo) {
            UWORD n = p->image->cbuf_words[si];
            ULONG *c = claim(ctx->v3d, n * 4UL + 16, 16);
            if (!c)
                return FALSE;
            memcpy(c, p->cbuf_le[si], n * 4UL);
            ts->ubo[si] = (ULONG)c;
        }
    }
    return TRUE;
}

/* Register spilling (Mesa's QUNIFORM_SPILL_*): a shader that ran out of
 * registers keeps values in a scratch area through the TMU (dEQP-GLES2's
 * dynamically indexed arrays), a slice per QPU thread. The context's area,
 * as Mesa's v3d_update_compiled_*: a shader picks its slice by TIDX,
 * (core << 6) | (qpu << 2) | thread, so QPUs x 4 slices of the largest spill
 * size drawn with yet. Only the GPU touches it. A smaller old area goes when
 * the open job has rendered (its draws may use it). */
static BOOL spill_area(struct OGLES2Context *ctx, const struct OGLES2Variant *const v[3])
{
    struct OGLES2Draw *d = ctx->draw;
    ULONG need = 0;
    int k;
    for (k = 0; k < 3; k++)
        if (v[k]->spill_size > need)
            need = v[k]->spill_size;
    if (need <= d->spill_size)
        return TRUE;
    if (d->spill.hostptr) {
        v3d_frame_defer_free(&ctx->v3d->frame, &d->spill);
        memset(&d->spill, 0, sizeof(d->spill));
    }
    d->spill_size = 0;
    if (v3d_mem_alloc(ogles2_device(), &d->spill,
                      (ULONG)ogles2_device()->deviceInfo.qpu_count * 4 * need + 256) < 0) {
        memset(&d->spill, 0, sizeof(d->spill));
        return FALSE;
    }
    d->spill_size = need;
    ogles2_log("draw: a shader spills registers: a %ld-byte area (%ld bytes a thread)",
               (LONG)d->spill.size, (LONG)need);
    return TRUE;
}

/* The image's sampler uniforms, listed at the program's first draw (most of
 * a fixed-function emulation's uniforms are not samplers): how many to look
 * at, p->samplers[k] or (0xfe, too many to list) every uniform. */
static UWORD list_samplers(struct OGLES2Program *p)
{
    const struct OGLES2Image *im = p->image;
    UWORD u, n;
    if (p->nsamplers == 0xff) {
        for (u = 0, n = 0; u < im->nuniforms && n <= 16; u++)
            if (im->uniforms[u].flags & OGLES2_UNIFORM_SAMPLER) {
                if (n < 16)
                    p->samplers[n] = u;
                n++;
            }
        p->nsamplers = n <= 16 ? (UBYTE)n : 0xfe;
    }
    return p->nsamplers == 0xfe ? im->nuniforms : p->nsamplers;
}

/* The texture unit of element e of sampler uniform u (glUniform1i). */
static UBYTE unit_of(const struct OGLES2Program *p, UWORD u, UWORD e)
{
    return p->uniform_loc[u] == 0xFFFF ? 0 : p->sampler_unit[p->uniform_loc[u] + e];
}

/* A fragment-shader sampler of the program reads a texture the TMU returns
 * 32 bits for (a float or depth texture): the draw needs the 32-bit
 * variants (Mesa's sampler_is_32b key). */
static BOOL wants_wide(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    const struct OGLES2Image *im = p->image;
    UWORD k, n = list_samplers(p);
    for (k = 0; k < n; k++) {
        UWORD u = p->nsamplers == 0xfe ? k : p->samplers[k];
        const struct OGLES2ImageUniform *un = &im->uniforms[u];
        UWORD e, ne = un->array ? un->array : 1;
        if (!(un->flags & OGLES2_UNIFORM_SAMPLER) || un->sampler[1] >= OGLES2_MAX_STAGE_SAMPLERS)
            continue;
        for (e = 0; e < ne; e++)
            if (ogles2_texture_wide(ctx, unit_of(p, u, e), ogles2_sampler_kind(un->type)))
                return TRUE;
    }
    return FALSE;
}

/* Every sampler uniform of the program: its texture index in each stage
 * that uses it reads the texture unit glUniform1i gave it. */
static BOOL texture_states(struct OGLES2Context *ctx, struct OGLES2Program *p,
                           struct tex_states *ts)
{
    const struct OGLES2Image *im = p->image;
    UWORD u, k, n;
    int st;
    n = list_samplers(p);
    for (k = 0; k < n; k++) {
        const struct OGLES2ImageUniform *un;
        u = p->nsamplers == 0xfe ? k : p->samplers[k];
        un = &im->uniforms[u];
        if (!(un->flags & OGLES2_UNIFORM_SAMPLER))
            continue;
        for (st = 0; st < 2; st++) {
            UWORD e, ne = un->array ? un->array : 1;
            for (e = 0; e < ne; e++) { /* an array: an element a texture index */
                ULONG idx = un->sampler[st] + e;
                if (un->sampler[st] >= OGLES2_MAX_STAGE_SAMPLERS || idx >= OGLES2_MAX_STAGE_SAMPLERS)
                    break;
                if (!ogles2_texture_state(ctx, unit_of(p, u, e), ogles2_sampler_kind(un->type),
                                          &ts->tex[st][idx], &ts->smp[st][idx]))
                    return FALSE;
            }
        }
    }
    return TRUE;
}

/* One shader's uniform stream from its recipe (plan section 8.6, Mesa's
 * v3d_write_uniforms); its address. One word more than the recipe: the
 * hardware prefetches the next uniform. */
static ULONG write_uniforms(struct OGLES2Context *ctx, const struct OGLES2Program *p,
                            const struct OGLES2Variant *var, const struct tex_states *tst)
{
    int si = var->stage == OGLES2_VARIANT_FS ? 1 : 0;
    const ULONG *cle = p->cbuf_le[si];
    const ULONG *rtype = var->rtype, *rdata = var->rdata;
    const struct OGLES2State *s = &ctx->state;
    float zs, zo;
    ULONG *u;
    int i, k, n;
    if (!(u = claim(ctx->v3d, (var->nrecipe + 1) * 4UL, 4)))
        return 0;
    viewport_z(ctx, &zs, &zo);
    ctx->n_uwords += var->nrecipe;
    /* Most of a stream is constant buffer words, already little-endian:
     * every entry is copied as one (the others from word 0), then the
     * others are written over. The built-in image has no such list. */
    if (var->uidx) {
        const UWORD *ui = var->uidx;
        ULONG *dst = u;
        for (k = var->nrecipe; k > 0; k--)
            *dst++ = cle[*ui++];
        n = var->nspecial;
    } else {
        n = var->nrecipe;
    }
    for (k = 0; k < n; k++) {
        ULONG d, w;
        i = var->uidx ? var->special[k] : k;
        d = rdata[i];
        switch (rtype[i]) {
        case OGLES2_U_UNIFORM:           w = p->cbuf[si][d]; break;
        case OGLES2_U_CONSTANT:          w = d; break;
        /* The default framebuffer is drawn upside down (plan 7.11): a
         * negative Y scale, as Mesa's state tracker gives a window; a
         * framebuffer object is not (y_down). */
        case OGLES2_U_VIEWPORT_X_SCALE:  w = float_bits(ctx->viewport[2] * 0.5f * 256.0f); break;
        case OGLES2_U_VIEWPORT_Y_SCALE:
            w = float_bits(ctx->viewport[3] * (y_down(ctx) ? -0.5f : 0.5f) * 256.0f);
            break;
        case OGLES2_U_VIEWPORT_Z_SCALE:  w = float_bits(zs); break;
        case OGLES2_U_VIEWPORT_Z_OFFSET: w = float_bits(zo); break;
        case OGLES2_U_CLIP_Z_COEFF:
            w = float_bits(s->clip_depth == GL_ZERO_TO_ONE_EXT ? -1.0f : 0.0f);
            break;
        case OGLES2_U_LINE_WIDTH:
        case OGLES2_U_AA_LINE_WIDTH:     w = float_bits(s->line_width); break;
        case OGLES2_U_BLEND_CONSTANT_R:
        case OGLES2_U_BLEND_CONSTANT_R + 1:
        case OGLES2_U_BLEND_CONSTANT_R + 2:
        case OGLES2_U_BLEND_CONSTANT_R + 3:
            w = float_bits(s->blend_color[var->rtype[i] - OGLES2_U_BLEND_CONSTANT_R]);
            break;
        /* Mesa's write_tmu_p0/p1: the state's address with the
         * compiler's configuration bits in its low bits. */
        case OGLES2_U_TMU_CONFIG_P0:
            w = tst->tex[si][(d >> 24) & (OGLES2_MAX_STAGE_SAMPLERS - 1)] | (d & 0xffffff);
            break;
        case OGLES2_U_TMU_CONFIG_P1:
            w = tst->smp[si][(d >> 24) & (OGLES2_MAX_STAGE_SAMPLERS - 1)] | (d & 0xffffff);
            break;
        case OGLES2_U_UBO_ADDR:          w = tst->ubo[si] + (d & 0xffffff); break;
        case OGLES2_U_SPILL_OFFSET:
            w = (ULONG)V3D_ALIGN_UP((v3d_uintptr)ctx->draw->spill.hostptr, 256);
            break;
        case OGLES2_U_SPILL_SIZE_PER_THREAD: w = ctx->draw->spill_size; break;
        case OGLES2_U_PISTORM_VP_WIDTH:  w = float_bits((GLfloat)ctx->viewport[2]); break;
        case OGLES2_U_PISTORM_VP_HEIGHT: w = float_bits((GLfloat)ctx->viewport[3]); break;
        default:                         w = 0; break; /* the loader refuses the rest */
        }
        u[i] = le32(w);
    }
    u[var->nrecipe] = 0;
    return (ULONG)u;
}

/* The coordinate or vertex shader's stream (k 0 or 1): the one its last
 * draw in this job wrote, when nothing it reads has changed since. The
 * fragment shader's samples the draw's textures: always written. */
static ULONG vertex_uniforms(struct OGLES2Context *ctx, struct OGLES2Program *p, int k,
                             const struct OGLES2Variant *var, const struct tex_states *tst)
{
    ULONG addr;
    if (p->us_var[k] == var && p->us_gen[k] == g_v3d_cl_generation && p->us_ctx[k] == ctx &&
        p->us_cbuf_ver[k] == p->cbuf_ver[0] && p->us_state_ver[k] == ctx->ustate_ver)
        return p->us_addr[k];
    addr = write_uniforms(ctx, p, var, tst);
    if (addr && var->no_textures && var->uidx) {
        p->us_var[k] = var;
        p->us_gen[k] = g_v3d_cl_generation;
        p->us_ctx[k] = ctx;
        p->us_cbuf_ver[k] = p->cbuf_ver[0];
        p->us_state_ver[k] = ctx->ustate_ver;
        p->us_addr[k] = addr;
    }
    return addr;
}

/* The state write_uniforms reads besides the constant buffers and the
 * textures (the blend colour only in a fragment shader's): a new version
 * when it changes. */
static void uniform_state(struct OGLES2Context *ctx)
{
    const struct OGLES2State *s = &ctx->state;
    ULONG u0 = (ULONG)ctx->viewport[2], u1 = (ULONG)ctx->viewport[3];
    ULONG u2 = float_bits(s->depth_range[0]), u3 = float_bits(s->depth_range[1]);
    ULONG u4 = float_bits(s->line_width), u5 = ctx->fb_flip;
    ULONG u6 = (s->clip_origin == GL_UPPER_LEFT_EXT) | (s->clip_depth == GL_ZERO_TO_ONE_EXT) << 1;
    ULONG *c = ctx->ustate;
    if (c[0] != u0 || c[1] != u1 || c[2] != u2 || c[3] != u3 || c[4] != u4 || c[5] != u5 || c[6] != u6) {
        c[0] = u0; c[1] = u1; c[2] = u2; c[3] = u3; c[4] = u4; c[5] = u5; c[6] = u6;
        ctx->ustate_ver++;
    }
}

/* The 16 default attribute values (0, 0, 0, 1), once per control list. */
static ULONG default_attribs(struct OGLES2Context *ctx, struct OGLES2Draw *dd)
{
    ULONG *p;
    int i;
    if (dd->defattr_gen == g_v3d_cl_generation)
        return dd->defattr;
    if (!(p = claim(ctx->v3d, 16 * 16, 16)))
        return 0;
    for (i = 0; i < 16; i++) {
        p[i * 4 + 0] = p[i * 4 + 1] = p[i * 4 + 2] = 0;
        p[i * 4 + 3] = le32(float_bits(1.0f));
    }
    dd->defattr_gen = g_v3d_cl_generation;
    dd->defattr = (ULONG)p;
    return dd->defattr;
}

struct attr_src
{
    ULONG address, stride;
    UBYTE vsize, vtype, normalized, is_signed;
};

/* Emu68 runs a memory access as dearly as several register operations:
 * movem moves four longs with one (tests/perf/emuspeed: 100 words swapped
 * 1.1 us a long at a time, 0.23 us with movem). Byte order swapped in
 * registers in between. */
#define SWAP4 "ror.w #8,%%d0\n\tswap %%d0\n\tror.w #8,%%d0\n\t" \
              "ror.w #8,%%d1\n\tswap %%d1\n\tror.w #8,%%d1\n\t" \
              "ror.w #8,%%d2\n\tswap %%d2\n\tror.w #8,%%d2\n\t" \
              "ror.w #8,%%d3\n\tswap %%d3\n\tror.w #8,%%d3\n\t"

/* n longs from s to d in the other byte order. */
static void swap_longs(ULONG *d, const ULONG *s, ULONG n)
{
    ULONG q;
    for (q = n >> 2; q; q--)
        __asm__ volatile("movem.l (%0)+,%%d0-%%d3\n\t" SWAP4
                         "movem.l %%d0-%%d3,(%1)\n\t"
                         "lea (16,%1),%1"
                         : "+a"(s), "+a"(d) : : "d0", "d1", "d2", "d3", "memory");
    for (n &= 3; n; n--)
        *d++ = le32(*s++);
}

/* n 32-bit indices less `base`, in the other byte order. */
static void swap_indices(ULONG *d, const ULONG *s, ULONG n, ULONG base)
{
    ULONG q;
    for (q = n >> 2; q; q--)
        __asm__ volatile("movem.l (%0)+,%%d0-%%d3\n\t"
                         "sub.l %2,%%d0\n\tsub.l %2,%%d1\n\tsub.l %2,%%d2\n\tsub.l %2,%%d3\n\t" SWAP4
                         "movem.l %%d0-%%d3,(%1)\n\t"
                         "lea (16,%1),%1"
                         : "+a"(s), "+a"(d) : "d"(base) : "d0", "d1", "d2", "d3", "memory");
    for (n &= 3; n; n--)
        *d++ = le32(*s++ - base);
}

/* `count` elements of `n` components of `type` into the GPU's byte order. */
static void convert_elems(UBYTE *dst, ULONG dst_stride, const UBYTE *src, ULONG src_stride,
                          ULONG count, GLint n, GLenum type)
{
    ULONG i;
    int k;
    switch (type) {
    case GL_FLOAT:
    case GL_FIXED:
        /* Word loads straight from the array (the 68020 reads any address;
         * -fno-strict-aliasing): the copy is most of a draw's CPU time. */
        if (src_stride == (ULONG)n * 4 && dst_stride == src_stride) {
            swap_longs((ULONG *)dst, (const ULONG *)src, count * (ULONG)n);
            break;
        }
        /* Strided (a position in a 4-float slot, interleaved arrays): one
         * movem each way a vertex. */
        if (n == 4) {
            for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
                __asm__ volatile("movem.l (%0),%%d0-%%d3\n\t" SWAP4 "movem.l %%d0-%%d3,(%1)"
                                 : : "a"(src), "a"(dst) : "d0", "d1", "d2", "d3", "memory");
            break;
        }
        if (n == 3) {
            for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
                __asm__ volatile("movem.l (%0),%%d0-%%d2\n\t"
                                 "ror.w #8,%%d0\n\tswap %%d0\n\tror.w #8,%%d0\n\t"
                                 "ror.w #8,%%d1\n\tswap %%d1\n\tror.w #8,%%d1\n\t"
                                 "ror.w #8,%%d2\n\tswap %%d2\n\tror.w #8,%%d2\n\t"
                                 "movem.l %%d0-%%d2,(%1)"
                                 : : "a"(src), "a"(dst) : "d0", "d1", "d2", "memory");
            break;
        }
        if (n == 2) {
            for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
                __asm__ volatile("movem.l (%0),%%d0-%%d1\n\t"
                                 "ror.w #8,%%d0\n\tswap %%d0\n\tror.w #8,%%d0\n\t"
                                 "ror.w #8,%%d1\n\tswap %%d1\n\tror.w #8,%%d1\n\t"
                                 "movem.l %%d0-%%d1,(%1)"
                                 : : "a"(src), "a"(dst) : "d0", "d1", "memory");
            break;
        }
        for (i = 0; i < count; i++, src += src_stride, dst += dst_stride) {
            const ULONG *s = (const ULONG *)src;
            ULONG *d = (ULONG *)dst;
            for (k = 0; k < n; k++)
                d[k] = le32(s[k]);
        }
        break;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
        for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
            for (k = 0; k < n; k++) {
                dst[k * 2] = src[k * 2 + 1];
                dst[k * 2 + 1] = src[k * 2];
            }
        break;
    default: /* bytes: no swap */
        if (n == 4 && src_stride == 4 && dst_stride == 4) { /* packed colours: movem (memcpy) */
            memcpy(dst, src, count * 4);
            break;
        }
        if (n == 4) { /* colours: one word each, no memcpy call */
            for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
                *(ULONG *)dst = *(const ULONG *)src;
            break;
        }
        for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
            memcpy(dst, src, (ULONG)n);
        break;
    }
}

/* Copies [first, first+count) of an enabled array in the GPU's byte order,
 * tightly packed at a 4-byte stride: a client array into the state buffer
 * per draw (plan 7.5), a buffer's through its kept GPU copy (buffer.c). */
static BOOL copy_array(struct OGLES2Context *ctx, const struct OGLES2Array *a, GLint first,
                       GLsizei count, struct attr_src *out, BOOL probe_addr)
{
    int es = type_size(a->type), n = a->size;
    ULONG src_stride = a->stride ? (ULONG)a->stride : (ULONG)(n * es);
    ULONG dst_stride = ((ULONG)(n * es) + 3) & ~3UL;
    const UBYTE *src = (const UBYTE *)a->pointer;
    UBYTE *dst;

    out->vsize = n == 4 ? v3d_VEC_4 : (UBYTE)n;
    out->normalized = a->normalized && a->type != GL_FLOAT && a->type != GL_FIXED;
    out->is_signed = a->type == GL_BYTE || a->type == GL_SHORT;
    /* GL_FIXED is read by the attribute record's fixed type, which is GL's
     * 16.16 (M1 probe F on the A1200, plan 7.5): a byte swap only. */
    out->vtype = a->type == GL_FIXED ? v3d_ATTRIBUTE_FIXED
               : a->type == GL_FLOAT ? v3d_ATTRIBUTE_FLOAT
               : es == 2 ? v3d_ATTRIBUTE_SHORT : v3d_ATTRIBUTE_BYTE;
    /* A buffer the program changes as it draws: the vertices this draw reads,
     * from its shadow (the draw checked the range against the buffer). */
    if (a->buffer && (src = ogles2_buffer_stream(a->buffer)) != 0)
        src += (ULONG)a->pointer;
    else if (a->buffer) {
        ULONG vertices, base = ogles2_buffer_copy(ctx, a->buffer, (ULONG)a->pointer, n, a->type,
                                                  out->normalized, a->stride, (ULONG)(n * es),
                                                  &out->stride, &vertices, convert_elems);
        /* The draw checked the range against the buffer (arrays_ok). */
        if (!base || (ULONG)first + (ULONG)count > vertices)
            return FALSE;
        out->address = base + (ULONG)first * out->stride;
        return TRUE;
    }
    if (probe_addr) {
        /* Probe A: the test's buffer, wherever it lies (4 KB). */
        if (dst_stride * (ULONG)count > 4096)
            return FALSE;
        dst = (UBYTE *)ctx->probe_addr;
    } else if (!(dst = claim(ctx->v3d, dst_stride * (ULONG)count, 4)))
        return FALSE;
    out->address = (ULONG)dst;
    out->stride = dst_stride;
    ctx->n_vbytes += dst_stride * (ULONG)count;
    convert_elems(dst, dst_stride, src + (ULONG)first * src_stride, src_stride, (ULONG)count, n, a->type);
    if (probe_addr) {
        ULONG len = dst_stride * (ULONG)count;
        CachePreDMA((APTR)out->address, &len, 0);
    }
    return TRUE;
}

/* A disabled array reads the current generic attribute: 16 bytes, stride 0. */
static BOOL constant_attrib(struct OGLES2Context *ctx, GLuint index, struct attr_src *out)
{
    ULONG *p;
    int k;
    if (!(p = claim(ctx->v3d, 16, 16)))
        return FALSE;
    for (k = 0; k < 4; k++)
        p[k] = le32(float_bits(ctx->state.attrib[index][k]));
    out->address = (ULONG)p;
    out->stride = 0;
    out->vsize = v3d_VEC_4;
    out->vtype = v3d_ATTRIBUTE_FLOAT;
    out->normalized = out->is_signed = 0;
    return TRUE;
}

/* One GL_SHADER_STATE_ATTRIBUTE_RECORD (16 bytes): v3dcore's
 * glShaderStateAttributeRecord's bytes, written directly (packets_ok checks
 * them once; it is used while they agree). */
static void put_le32(UBYTE *b, ULONG x);
static BOOL packets_ok(V3DContext *v);
static int column_slot(const struct OGLES2ImageAttrib *at, int k);

static void attr_record(UBYTE *b, ULONG address, UBYTE normalized, UBYTE is_signed, UBYTE vsize,
                        UBYTE vtype, UBYTE vs_reads, UBYTE cs_reads, ULONG stride, ULONG maxindex)
{
    put_le32(b, address);
    b[4] = (UBYTE)((normalized ? 0x40 : 0) | (is_signed ? 0x20 : 0) | ((vtype & 7) << 2) | (vsize & 3));
    b[5] = (UBYTE)((vs_reads << 4) | (cs_reads & 15));
    b[6] = b[7] = 0; /* instance divisor */
    put_le32(b + 8, stride);
    put_le32(b + 12, maxindex);
}

/* The GL shader state record and its attribute records, one per vertex
 * input slot (Mesa's emit_shader_state_record and v3d_emit_gl_shader_state,
 * V3D 4.2, no geometry shader); the record's address. */
static ULONG shader_record(struct OGLES2Context *ctx, struct OGLES2Program *p, ULONG defattr,
                           const ULONG code[3], const ULONG unif[3], const struct OGLES2Variant *cs,
                           const struct OGLES2Variant *vs, const struct OGLES2Variant *fs,
                           GLenum mode, const struct attr_src *src, int nslots)
{
    V3DContext *v = ctx->v3d;
    int nrec = nslots ? nslots : 1;
    ULONG size = sizeof(v3d_gl_shader_state_record) +
                 nrec * sizeof(v3d_gl_shader_state_attribute_record);
    v3d_gl_shader_state_record *r = claim(v, size, 32); /* GL_SHADER_STATE holds address >> 5 */
    v3d_static_buffer attrs, *saved;
    ULONG *swivel;
    BOOL direct = packets_ok(v);
    BOOL cs_loaded_any = FALSE;
    BOOL cs_builtins = cs->uses_iid || cs->uses_vid;
    int i;

    BOOL points = mode == GL_POINTS && vs->per_vertex_point_size;

    if (!r)
        return 0;
    /* The program's template when it was made for these variants: only
     * the four addresses below differ from draw to draw. */
    if (p->rec_tmpl && p->rec_var[0] == cs && p->rec_var[1] == vs && p->rec_var[2] == fs &&
        p->rec_points == points && p->rec_code[0] == code[0] && p->rec_code[1] == code[1] &&
        p->rec_code[2] == code[2]) {
        memcpy(r, p->rec_tmpl, sizeof(*r));
        goto addresses;
    }
    memset(r, 0, size);
    r->enable_clipping = TRUE;
    r->point_size_in_shaded_vertex_data = points;
    r->fragment_shader_does_z_writes = fs->writes_z;
    r->turn_off_early_z_test = fs->disable_ez;
    r->fragment_shader_uses_real_pixel_centre_w_in_addition_to_centroid_w2 = fs->uses_center_w;
    r->any_shader_reads_hardware_written_primitive_id = fs->uses_pid;
    r->insert_primitive_id_as_first_varying_to_fragment_shader = fs->uses_pid;
    r->do_scoreboard_wait_on_first_thread_switch = fs->lock_scoreboard_on_first_thrsw;
    r->disable_implicit_point_line_varyings = !fs->uses_implicit_point_line_varyings;
    r->number_of_varyings_in_fragment_shader = fs->num_inputs;
    r->coordinate_shader_has_separate_input_and_output_vpm_blocks = cs->separate_segments;
    r->vertex_shader_has_separate_input_and_output_vpm_blocks = vs->separate_segments;
    r->coordinate_shader_input_vpm_segment_size = cs->separate_segments ? cs->vpm_input_size : 1;
    r->vertex_shader_input_vpm_segment_size = vs->separate_segments ? vs->vpm_input_size : 1;
    r->coordinate_shader_output_vpm_segment_size = cs->vpm_output_size;
    r->vertex_shader_output_vpm_segment_size = vs->vpm_output_size;
    /* v3d_compute_vpm_config without a geometry shader: As = 1 (stored
     * minus one), Ve = 0, and Vc from VCM_CACHE_SIZE: the zeroes stay. */
    r->vertex_id_read_by_coordinate_shader = cs->uses_vid;
    r->instance_id_read_by_coordinate_shader = cs->uses_iid;
    r->vertex_id_read_by_vertex_shader = vs->uses_vid;
    r->instance_id_read_by_vertex_shader = vs->uses_iid;

    r->fragment_shader_code_address_rshift_3 = code[2] >> 3;
    r->fragment_shader_4_way_threadable = fs->threads == 4;
    r->fragment_shader_start_in_final_thread_section = fs->single_seg;
    r->fragment_shader_propagate_nans = TRUE;

    r->vertex_shader_code_address_rshift_3 = code[1] >> 3;
    r->vertex_shader_4_way_threadable = vs->threads == 4;
    r->vertex_shader_start_in_final_thread_section = vs->single_seg;
    r->vertex_shader_propagate_nans = TRUE;

    r->coordinate_shader_code_address_rshift_3 = code[0] >> 3;
    r->coordinate_shader_4_way_threadable = cs->threads == 4;
    r->coordinate_shader_start_in_final_thread_section = cs->single_seg;
    r->coordinate_shader_propagate_nans = TRUE;

    /* The bitfield words holding the code addresses, as PiStorm3D swaps them. */
    swivel = (ULONG *)r;
    swivel[3] = le32(swivel[3]);
    swivel[5] = le32(swivel[5]);
    swivel[7] = le32(swivel[7]);
    if (!p->rec_tmpl)
        p->rec_tmpl = AllocVec(sizeof(*r), MEMF_ANY);
    if (p->rec_tmpl) {
        memcpy(p->rec_tmpl, r, sizeof(*r));
        p->rec_var[0] = cs;
        p->rec_var[1] = vs;
        p->rec_var[2] = fs;
        p->rec_points = points;
        p->rec_code[0] = code[0];
        p->rec_code[1] = code[1];
        p->rec_code[2] = code[2];
    }

addresses:
    /* Whole little-endian words, outside the swapped ones above. */
    r->address_of_default_attribute_values = le32(defattr);
    r->fragment_shader_uniforms_address = le32(unif[2]);
    r->vertex_shader_uniforms_address = le32(unif[1]);
    r->coordinate_shader_uniforms_address = le32(unif[0]);

    /* The attribute records follow the record directly, in input slot order. */
    attrs.start = (v3d_u8 *)(r + 1);
    attrs.used = 0;
    attrs.capacity = nrec * sizeof(v3d_gl_shader_state_attribute_record);
    attrs.overflowed = 0;
    saved = v->current_buf;
    v->current_buf = &attrs;
    if (!nslots) {
        /* GFXH-930: at least one attribute read by both shaders: a dummy. */
        glShaderStateAttributeRecord(v, (APTR)defattr, FALSE, FALSE, FALSE, v3d_VEC_1,
                                     v3d_ATTRIBUTE_FLOAT, 1, 1, 0, 0, 0xFFFFFF);
    }
    for (i = 0; i < nslots; i++) {
        UBYTE cs_n = cs->vattr_sizes[i];
        /* GFXH-930 and GFXH-1602 (Mesa): the binning shader loads at least
         * one attribute, the first when it reads the vertex/instance id. */
        if (cs_n)
            cs_loaded_any = TRUE;
        if (i == 0 && cs_builtins && !cs_loaded_any) {
            cs_n = 1;
            cs_loaded_any = TRUE;
        } else if (i == nslots - 1 && !cs_loaded_any) {
            cs_n = 1;
            cs_loaded_any = TRUE;
        }
        if (direct)
            attr_record((UBYTE *)(r + 1) + i * sizeof(v3d_gl_shader_state_attribute_record), src[i].address,
                        src[i].normalized, src[i].is_signed, src[i].vsize, src[i].vtype, vs->vattr_sizes[i],
                        cs_n, src[i].stride, 0xFFFFFF);
        else
            glShaderStateAttributeRecord(v, (APTR)src[i].address, FALSE, src[i].normalized,
                                         src[i].is_signed, src[i].vsize, src[i].vtype,
                                         vs->vattr_sizes[i], cs_n, 0, src[i].stride, 0xFFFFFF);
    }
    v->current_buf = saved;
    return (ULONG)r;
}

/* ---- state packets ---- */

static UBYTE blend_factor(GLenum f)
{
    switch (f) {
    case GL_ZERO:                     return V3D_BLEND_FACTOR_ZERO;
    case GL_ONE:                      return V3D_BLEND_FACTOR_ONE;
    case GL_SRC_COLOR:                return V3D_BLEND_FACTOR_SRCCOLOR;
    case GL_ONE_MINUS_SRC_COLOR:      return V3D_BLEND_FACTOR_INVSRCCOLOR;
    case GL_DST_COLOR:                return V3D_BLEND_FACTOR_DSTCOLOR;
    case GL_ONE_MINUS_DST_COLOR:      return V3D_BLEND_FACTOR_INVDSTCOLOR;
    case GL_SRC_ALPHA:                return V3D_BLEND_FACTOR_SRCALPHA;
    case GL_ONE_MINUS_SRC_ALPHA:      return V3D_BLEND_FACTOR_INVSRCALPHA;
    case GL_DST_ALPHA:                return V3D_BLEND_FACTOR_DSTALPHA;
    case GL_ONE_MINUS_DST_ALPHA:      return V3D_BLEND_FACTOR_INVDSTALPHA;
    case GL_CONSTANT_COLOR:           return V3D_BLEND_FACTOR_CONSTCOLOR;
    case GL_ONE_MINUS_CONSTANT_COLOR: return V3D_BLEND_FACTOR_INVCONSTCOLOR;
    case GL_CONSTANT_ALPHA:           return V3D_BLEND_FACTOR_CONSTALPHA;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return V3D_BLEND_FACTOR_INVCONSTALPHA;
    default:                          return V3D_BLEND_FACTOR_SRCALPHASATURATE;
    }
}

static UBYTE blend_mode(GLenum e)
{
    return e == GL_FUNC_SUBTRACT ? V3D_BLEND_MODE_SUB
         : e == GL_FUNC_REVERSE_SUBTRACT ? V3D_BLEND_MODE_RSUB
         : e == GL_MIN_EXT ? V3D_BLEND_MODE_MIN
         : e == GL_MAX_EXT ? V3D_BLEND_MODE_MAX : V3D_BLEND_MODE_ADD;
}

/* EXT_blend_minmax ignores the factors; Mesa's state tracker
 * (st_atom_blend.c) sets them to ONE for MIN and MAX, as here. */
static UBYTE blend_factor_for(GLenum f, GLenum e)
{
    return e == GL_MIN_EXT || e == GL_MAX_EXT ? V3D_BLEND_FACTOR_ONE : blend_factor(f);
}

/* IEEE half from a float in [0, 1] (blend constant colour). */
static UWORD half(GLfloat f)
{
    ULONG u = float_bits(f), mant;
    LONG e;
    if (f <= 0.0f)
        return 0;
    e = (LONG)((u >> 23) & 0xff) - 127 + 15;
    mant = u & 0x7fffff;
    if (e <= 0) /* denormal */
        return (UWORD)((mant | 0x800000) >> (14 - e));
    return (UWORD)((e << 10) | (mant >> 13));
}

static UBYTE stencil_op(GLenum op)
{
    switch (op) {
    case GL_ZERO:      return V3D_STENCIL_OP_ZERO;
    case GL_REPLACE:   return V3D_STENCIL_OP_REPLACE;
    case GL_INCR:      return V3D_STENCIL_OP_INCR;
    case GL_DECR:      return V3D_STENCIL_OP_DECR;
    case GL_INVERT:    return V3D_STENCIL_OP_INVERT;
    case GL_INCR_WRAP: return V3D_STENCIL_OP_INCWRAP;
    case GL_DECR_WRAP: return V3D_STENCIL_OP_DECWRAP;
    default:           return V3D_STENCIL_OP_KEEP;
    }
}

/* STENCIL_CFG (opcode 80) for one face, packed byte by byte from Mesa's
 * v3d_packet.xml (plan 4.3): ref bits 0-7, test mask 8-15, function 16-18,
 * stencil fail op 19-21, depth fail op 22-24, pass op 25-27, front 28,
 * back 29, write mask 32-39. */
static void stencil_bytes(const struct OGLES2State *s, int face, UBYTE *b)
{
    GLint ref = s->stencil_ref[face];
    ULONG w = (ULONG)(s->stencil_func[face] - GL_NEVER) |
              ((ULONG)stencil_op(s->stencil_fail[face]) << 3) |
              ((ULONG)stencil_op(s->stencil_zfail[face]) << 6) |
              ((ULONG)stencil_op(s->stencil_zpass[face]) << 9) |
              (face ? 1UL << 13 : 1UL << 12);
    b[0] = (UBYTE)(ref < 0 ? 0 : ref > 255 ? 255 : ref); /* clamped to the 8 bits (GLES 2.0 4.1.4) */
    b[1] = (UBYTE)s->stencil_value_mask[face];
    b[2] = (UBYTE)w;
    b[3] = (UBYTE)(w >> 8);
    b[4] = (UBYTE)s->stencil_write_mask[face];
}

static void emit_stencil(V3DContext *v, const UBYTE *b)
{
    UBYTE *p = v3d_buffer_claim_memory(v->current_buf, 6);
    if (p) {
        p[0] = v3d_OP_STENCIL_CFG;
        memcpy(p + 1, b, 5);
    }
}

/* Mesa's per-frame early-Z direction (v3d_update_job_ez), from this draw's
 * depth and stencil state; the frame keeps one direction or gives early Z up. */
static void update_ez(V3DContext *v, BOOL depth_on, GLenum func, BOOL stencil_on,
                      const struct OGLES2State *s)
{
    UBYTE draw_ez = V3D_EZ_UNDECIDED;
    if (depth_on) {
        switch (func) {
        case GL_LESS: case GL_LEQUAL:    draw_ez = V3D_EZ_LT_LE; break;
        case GL_GREATER: case GL_GEQUAL: draw_ez = V3D_EZ_GT_GE; break;
        case GL_NEVER: case GL_EQUAL:    draw_ez = V3D_EZ_UNDECIDED; break;
        default:                         draw_ez = V3D_EZ_DISABLED; break;
        }
        /* Mesa (v3dx_state.c): a stencil test that can reject, or that
         * changes values when depth fails, breaks early-Z updates. */
        if (stencil_on && (s->stencil_zfail[0] != GL_KEEP || s->stencil_func[0] != GL_ALWAYS ||
                           s->stencil_zfail[1] != GL_KEEP || s->stencil_func[1] != GL_ALWAYS))
            draw_ez = V3D_EZ_DISABLED;
    }
    if (draw_ez == V3D_EZ_DISABLED)
        v->ez_state = V3D_EZ_DISABLED;
    else if (draw_ez != V3D_EZ_UNDECIDED) {
        if (v->ez_state == V3D_EZ_UNDECIDED)
            v->ez_state = draw_ez;
        else if (v->ez_state != draw_ez)
            v->ez_state = V3D_EZ_DISABLED;
    }
    /* The render config takes the first decided direction; a job whose first
     * draw disables early Z keeps it off (Mesa: draw_calls_queued == 0). */
    if (v->first_ez_state == V3D_EZ_UNDECIDED &&
        (v->ez_state != V3D_EZ_DISABLED || !v->draw_state_configured))
        v->first_ez_state = v->ez_state;
}

static void emit_state(struct OGLES2Context *ctx, struct dedup *dd, int prim,
                       const struct OGLES2Variant *fs)
{
    V3DContext *v = ctx->v3d;
    const struct OGLES2State *s = &ctx->state;
    BOOL depth_on = (s->caps & OGLES2_CAP_DEPTH_TEST) && ctx->fb_depth_bits;
    BOOL stencil_on = (s->caps & OGLES2_CAP_STENCIL_TEST) && ctx->fb_stencil_bits;
    BOOL flip = ctx->fb_flip;
    UBYTE effp, erfp, cp, edo, dtf, zue, eze, ezue, be, se, wf, pf, pv, rom, smask;
    LONG fh = (LONG)ctx->fb_h;
    LONG vx = ctx->viewport[0], vy = ctx->viewport[1], vw = ctx->viewport[2], vh = ctx->viewport[3];
    LONG l, t, r, b;
    UWORD vp_cx2, vp_cy2;
    ULONG cwm;
    float zs, zo;
    int k;

    viewport_z(ctx, &zs, &zo);

    if (dd->gen != g_v3d_cl_generation) { /* a new control list: emit everything */
        memset(dd, 0, sizeof(*dd));
        dd->gen = g_v3d_cl_generation;
        dd->prim = -1;
    } else if (dd->ver == ctx->state_ver && dd->prim == prim && dd->fs == fs && dd->fb == ctx->draw_fb &&
               dd->fb_w == ctx->fb_w && dd->fb_h == ctx->fb_h && dd->fb_depth_bits == ctx->fb_depth_bits &&
               dd->fb_stencil_bits == ctx->fb_stencil_bits && v->draw_state_configured) {
        return; /* nothing it reads has changed since the last draw of this list */
    }
    dd->ver = ctx->state_ver;
    dd->fs = fs;
    dd->fb = ctx->draw_fb;
    dd->fb_w = ctx->fb_w;
    dd->fb_h = ctx->fb_h;
    dd->fb_depth_bits = (UBYTE)ctx->fb_depth_bits;
    dd->fb_stencil_bits = (UBYTE)ctx->fb_stencil_bits;

    /* CLIP_WINDOW: viewport, and scissor box if enabled, in the hardware's
     * top-down frame (the default framebuffer's GL y is from the bottom; a
     * framebuffer object's row 0 is GL's), clamped to the framebuffer. */
    l = vx;
    r = vx + vw;
    t = flip ? fh - (vy + vh) : vy;
    b = flip ? fh - vy : vy + vh;
    if (s->caps & OGLES2_CAP_SCISSOR_TEST) {
        LONG sl = ctx->scissor[0], sr = sl + ctx->scissor[2];
        LONG st = flip ? fh - (ctx->scissor[1] + ctx->scissor[3]) : ctx->scissor[1];
        LONG sb = flip ? fh - ctx->scissor[1] : ctx->scissor[1] + ctx->scissor[3];
        if (sl > l) l = sl;
        if (sr < r) r = sr;
        if (st > t) t = st;
        if (sb < b) b = sb;
    }
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (LONG)ctx->fb_w) r = (LONG)ctx->fb_w;
    if (b > fh) b = fh;
    if (r < l) r = l;
    if (b < t) b = t;
    if (!dd->cw || dd->cw_l != l || dd->cw_t != t || dd->cw_w != r - l || dd->cw_h != b - t) {
        ClipWindow(v, (UWORD)l, (UWORD)t, (UWORD)(r - l), (UWORD)(b - t));
        dd->cw = TRUE;
        dd->cw_l = (UWORD)l; dd->cw_t = (UWORD)t; dd->cw_w = (UWORD)(r - l); dd->cw_h = (UWORD)(b - t);
    }

    /* CFG_BITS. Culling as Mesa's v3dx_emit.c; the winding flag set for
     * counter-clockwise as PiStorm3D's on the flipped default framebuffer
     * (its shaders flip y the same way), for clockwise on an object, and
     * the other way round with an upper-left origin (y_down). */
    effp = erfp = TRUE;
    if (s->caps & OGLES2_CAP_CULL_FACE) {
        if (s->cull_face_mode != GL_BACK) effp = FALSE;  /* FRONT or FRONT_AND_BACK */
        if (s->cull_face_mode != GL_FRONT) erfp = FALSE; /* BACK or FRONT_AND_BACK */
    }
    cp = y_down(ctx) ? s->front_face == GL_CCW : s->front_face == GL_CW;
    edo = (s->caps & OGLES2_CAP_POLYGON_OFFSET_FILL) ? TRUE : FALSE;
    dtf = depth_on ? (UBYTE)(s->depth_func - GL_NEVER) : V3D_COMPARE_FUNC_ALWAYS;
    zue = depth_on && s->depth_mask;
    update_ez(v, depth_on, s->depth_func, stencil_on, s);
    /* Mesa: a fragment shader that computes its own Z can go against the
     * frame's early-Z direction. */
    if (fs->writes_z && !fs->writes_z_from_fep)
        v->ez_state = V3D_EZ_DISABLED;
    eze = depth_on && v->ez_state != V3D_EZ_DISABLED;
    ezue = eze;
    if (!depth_on && !zue) /* Mesa: updates follow the frame's EZ state outside the depth gate */
        ezue = v->ez_state != V3D_EZ_DISABLED;
    be = (s->caps & OGLES2_CAP_BLEND) ? TRUE : FALSE;
    se = stencil_on ? TRUE : FALSE;
    /* glPolygonMode and glProvokingVertex (AmigaOS 4), as Mesa's v3dx_emit.c:
     * the hardware applies the fill modes to triangles only. */
    wf = s->polygon_mode != GL_FILL;
    pf = s->polygon_mode == GL_POINT;
    pv = s->provoking_vertex == GL_FIRST_VERTEX_CONVENTION;
    /* 4x MSAA (the default framebuffer's job): the rasterizer samples four
     * points a pixel (Mesa v3dx_emit.c), and glSampleCoverage's mask, as
     * Mesa's state tracker makes one: round(value x 4) samples, inverted on
     * request. */
    rom = ctx->job_msaa ? 1 : 0;
    smask = 0xF;
    if (ctx->job_msaa && (s->caps & OGLES2_CAP_SAMPLE_COVERAGE)) {
        int n = (int)(s->sample_coverage_value * 4.0f + 0.5f);
        smask = (UBYTE)((1U << n) - 1);
        if (s->sample_coverage_invert)
            smask = (UBYTE)(~smask & 0xF);
    }
    if (!dd->cfg || dd->effp != effp || dd->erfp != erfp || dd->cp != cp || dd->edo != edo ||
        dd->dtf != dtf || dd->zue != zue || dd->eze != eze || dd->ezue != ezue || dd->be != be ||
        dd->se != se || dd->wf != wf || dd->pf != pf || dd->pv != pv || dd->rom != rom) {
        CfgBits(v, effp, erfp, cp, edo, V3D_LINE_RASTERIZATION_DIAMOND_EXIT, rom, wf, dtf, zue, eze,
                ezue, se, be, pf, pv);
        dd->cfg = TRUE;
        dd->effp = effp; dd->erfp = erfp; dd->cp = cp; dd->edo = edo; dd->dtf = dtf;
        dd->zue = zue; dd->eze = eze; dd->ezue = ezue; dd->be = be; dd->se = se;
        dd->wf = wf; dd->pf = pf; dd->pv = pv; dd->rom = rom;
    }
    /* STENCIL_CFG, front and back, while the test is on (Mesa v3dx_emit.c). */
    if (stencil_on) {
        UBYTE b[5];
        for (k = 0; k < 2; k++) {
            stencil_bytes(s, k, b);
            if (!dd->st || memcmp(b, dd->stencil[k], 5)) {
                emit_stencil(v, b);
                memcpy(dd->stencil[k], b, 5);
            }
        }
        dd->st = TRUE;
    }
    if (edo) {
        /* The GPU counts units in a 24-bit depth buffer's steps: x256 for a
         * 16-bit one (Mesa's depth_offset_z16; exact, a power of two). */
        GLfloat units = s->polygon_offset_units * (ctx->fb_depth_bits == 16 ? 256.0f : 1.0f);
        if (!dd->do_valid || dd->do_factor != s->polygon_offset_factor || dd->do_units != units) {
            DepthOffset(v, s->polygon_offset_factor, units);
            dd->do_valid = TRUE;
            dd->do_factor = s->polygon_offset_factor;
            dd->do_units = units;
        }
    }
    /* POINT_SIZE on every draw (Mesa's V3D 4.2 binner erratum workaround):
     * with the draw's other packets (draw_packets). */
    if (!dd->lw || dd->lw_value != s->line_width) {
        LineWidth(v, s->line_width);
        dd->lw = TRUE;
        dd->lw_value = s->line_width;
    }

    /* The viewport group (CLIPPER_XY_SCALING, the Z packets, VIEWPORT_OFFSET),
     * as Mesa's v3dx_emit.c: the Z scale and offset from the depth range and
     * mode, the clipper's Y scale of the vertex shader's sign. The Z planes
     * are the depth range in window depth, in either mode (Mesa's
     * util_viewport_zmin_zmax). */
    vp_cx2 = (UWORD)(2 * vx + vw);              /* twice the centre, from the left */
    vp_cy2 = (UWORD)(flip ? 2 * (fh - vy) - vh  /* twice the centre, from the top */
                          : 2 * vy + vh);
    if (!dd->vp || dd->vp_w != vw || dd->vp_h != vh || dd->vp_cx2 != vp_cx2 || dd->vp_cy2 != vp_cy2 ||
        dd->vp_zs != zs || dd->vp_zo != zo || dd->vp_down != y_down(ctx)) {
        ClipperXYScaling(v, (UWORD)vw, (UWORD)vh, y_down(ctx));
        ClipperZScaleAndOffset(v, zs, zo);
        ClipperZMinMaxClippingPlanes(v, zo - zs < zo + zs ? zo - zs : zo + zs,
                                     zo - zs < zo + zs ? zo + zs : zo - zs);
        ViewportOffset(v, vp_cx2, 0, vp_cy2, 0);
        dd->vp = TRUE;
        dd->vp_w = (UWORD)vw; dd->vp_h = (UWORD)vh; dd->vp_cx2 = vp_cx2; dd->vp_cy2 = vp_cy2;
        dd->vp_zs = zs; dd->vp_zo = zo; dd->vp_down = y_down(ctx);
    }

    /* Blending: BLEND_ENABLES, BLEND_CFG (all four render targets, as Mesa
     * and PiStorm3D), BLEND_CONSTANT_COLOR. */
    if (be) {
        UBYTE src = blend_factor_for(s->blend_src_rgb, s->blend_eq_rgb);
        UBYTE dst = blend_factor_for(s->blend_dst_rgb, s->blend_eq_rgb);
        UBYTE asrc = blend_factor_for(s->blend_src_alpha, s->blend_eq_alpha);
        UBYTE adst = blend_factor_for(s->blend_dst_alpha, s->blend_eq_alpha);
        UBYTE eq = blend_mode(s->blend_eq_rgb), aeq = blend_mode(s->blend_eq_alpha);
        UWORD c[4];
        for (k = 0; k < 4; k++)
            c[k] = half(s->blend_color[k]);
        if (!dd->blend || !dd->be || dd->b_src != src || dd->b_dst != dst || dd->b_eq != eq ||
            dd->b_asrc != asrc || dd->b_adst != adst || dd->b_aeq != aeq ||
            dd->b_color[0] != c[0] || dd->b_color[1] != c[1] || dd->b_color[2] != c[2] ||
            dd->b_color[3] != c[3]) {
            BlendEnables(v, 0x0F);
            BlendCfg(v, 0x0F, dst, src, eq, adst, asrc, aeq);
            BlendConstantColor(v, c[3], c[2], c[1], c[0]);
            dd->b_src = src; dd->b_dst = dst; dd->b_eq = eq;
            dd->b_asrc = asrc; dd->b_adst = adst; dd->b_aeq = aeq;
            for (k = 0; k < 4; k++)
                dd->b_color[k] = c[k];
        }
    } else if (!dd->blend || dd->be) {
        BlendEnables(v, 0x00);
    }
    dd->blend = TRUE;
    dd->be = be;

    /* COLOR_WRITE_MASKS: a set bit disables a channel (R, G, B, A = bits
     * 0-3), four bits a render target. GLES2's one mask for every draw
     * buffer; a target no draw buffer names (EXT_draw_buffers' GL_NONE, a
     * missing attachment) is not written at all. */
    {
        ULONG m = (s->color_mask[0] ? 0 : 1) | (s->color_mask[1] ? 0 : 2) |
                  (s->color_mask[2] ? 0 : 4) | (s->color_mask[3] ? 0 : 8);
        cwm = 0;
        for (k = 0; k < OGLES2_MAX_DRAW_BUFFERS; k++)
            cwm |= ((ctx->fb_draw_mask & (1 << k)) ? m : 0xF) << (4 * k);
    }
    if (!dd->cwm || dd->cwm_mask != cwm) {
        ColorWriteMasks(v, cwm);
        dd->cwm = TRUE;
        dd->cwm_mask = cwm;
    }

    /* The per-pass residue on a pass's first draw (PiStorm3D's
     * gl_EnsureDrawState; Mesa's first draw, v3dx_emit.c). */
    if (!v->draw_state_configured) {
        DoCommand(v, v3d_OP_ZERO_ALL_FLAT_SHADE_FLAGS);
        DoCommand(v, v3d_OP_ZERO_ALL_NON_PERSPECTIVE_FLAGS);
        DoCommand(v, v3d_OP_ZERO_ALL_CENTROID_FLAGS);
        TransformFeedbackSpecs(v, FALSE, 0);
        OcclusionQueryCounter(v, 0);
        SampleState(v, 1, smask); /* coverage 1.0 (the emitter converts it to a float) */
        dd->smask = smask;
        v->draw_state_configured = TRUE;
        dd->prim = prim;
    } else {
        if (dd->prim != prim) { /* Mesa: on every primitive-type change */
            TransformFeedbackSpecs(v, FALSE, 0);
            dd->prim = prim;
        }
        if (dd->smask != smask) { /* glSampleCoverage changed within the pass */
            SampleState(v, 1, smask);
            dd->smask = smask;
        }
    }
}

/* ---- the packets every draw ends with ---- */

static void put_le32(UBYTE *b, ULONG x)
{
    b[0] = (UBYTE)x;
    b[1] = (UBYTE)(x >> 8);
    b[2] = (UBYTE)(x >> 16);
    b[3] = (UBYTE)(x >> 24);
}

/* POINT_SIZE 1.0, VCM_CACHE_SIZE, GL_SHADER_STATE, then INDEX_BUFFER_SETUP
 * and INDEXED_PRIM_LIST (itype: v3d_INDEX_TYPE_*), or VERTEX_ARRAY_PRIMS
 * when itype < 0: v3dcore's writers' bytes (v3d_commands.c), written
 * directly. Theirs clear each packet a byte at a time and pack bitfields,
 * about a microsecond a draw on Emu68. 31 or 22 bytes; checked against
 * theirs once (packets_ok). */
static ULONG draw_packets(UBYTE *b, UBYTE vcm_cs, UBYTE vcm_vs, ULONG rec, int nrec, UBYTE mode,
                          int itype, ULONG idx_addr, ULONG idx_size, ULONG count)
{
    b[0] = v3d_OP_POINT_SIZE;
    put_le32(b + 1, 0x3F800000UL); /* 1.0f */
    b[5] = v3d_OP_VCM_CACHE_SIZE;
    b[6] = (UBYTE)((vcm_vs << 4) | (vcm_cs & 15)); /* rendering, binning */
    b[7] = v3d_OP_GL_SHADER_STATE;
    put_le32(b + 8, (rec & ~31UL) | ((ULONG)nrec & 31));
    if (itype >= 0) {
        b[12] = v3d_OP_INDEX_BUFFER_SETUP;
        put_le32(b + 13, idx_addr);
        put_le32(b + 17, idx_size);
        b[21] = v3d_OP_INDEXED_PRIM_LIST;
        b[22] = (UBYTE)((itype << 6) | (mode & 63));
        put_le32(b + 23, count & 0x7FFFFFFFUL); /* no primitive restart */
        put_le32(b + 27, 0);
        return 31;
    }
    b[12] = v3d_OP_VERTEX_ARRAY_PRIMS;
    b[13] = mode;
    put_le32(b + 14, count);
    put_le32(b + 18, 0);
    return 22;
}

/* draw_packets' bytes are v3dcore's: made both ways once, into a buffer of
 * the test's own, for both endings (0 not checked yet, 1 the same, 2 not:
 * logged, and v3dcore's writers used). */
static UBYTE packets_state;

static BOOL packets_ok(V3DContext *v)
{
    UBYTE ref[128], mine[128];
    v3d_static_buffer sb, *saved;
    ULONG n;
    if (packets_state)
        return packets_state == 1;
    memset(&sb, 0, sizeof(sb));
    memset(ref, 0, sizeof(ref));
    sb.start = ref;
    sb.capacity = sizeof(ref);
    saved = v->current_buf;
    v->current_buf = &sb;
    PointSize(v, 1.0f);
    VCMCacheSize(v, 3, 5);
    glShaderState(v, 0x12345660UL, 3);
    IndexBufferSetup(v, 0x0ABCDEF4UL, 0x1236);
    IndexedPrimList(v, 4, v3d_INDEX_TYPE_16_BIT, 0x10203, FALSE, 0);
    PointSize(v, 1.0f);
    VCMCacheSize(v, 2, 1);
    glShaderState(v, 0x0FEDCBA0UL, 1);
    VertexArrayPrims(v, 5, 0x30201, 0);
    glShaderStateAttributeRecord(v, (APTR)0x08123456UL, FALSE, TRUE, TRUE, v3d_VEC_3, v3d_ATTRIBUTE_SHORT,
                                 3, 2, 0, 0x0C, 0xFFFFFF);
    glShaderStateAttributeRecord(v, (APTR)0x08654320UL, FALSE, FALSE, FALSE, v3d_VEC_4, v3d_ATTRIBUTE_FLOAT,
                                 4, 1, 0, 0x10, 0xFFFFFF);
    v->current_buf = saved;
    n = draw_packets(mine, 3, 5, 0x12345660UL, 3, 4, v3d_INDEX_TYPE_16_BIT, 0x0ABCDEF4UL, 0x1236, 0x10203);
    n += draw_packets(mine + n, 2, 1, 0x0FEDCBA0UL, 1, 5, -1, 0, 0, 0x30201);
    attr_record(mine + n, 0x08123456UL, TRUE, TRUE, v3d_VEC_3, v3d_ATTRIBUTE_SHORT, 3, 2, 0x0C, 0xFFFFFF);
    n += 16;
    attr_record(mine + n, 0x08654320UL, FALSE, FALSE, v3d_VEC_4, v3d_ATTRIBUTE_FLOAT, 4, 1, 0x10, 0xFFFFFF);
    n += 16;
    packets_state = (ULONG)sb.used == n && !sb.overflowed && !memcmp(ref, mine, n) ? 1 : 2;
    if (packets_state == 2)
        ogles2_log("draw: the direct packet writer differs from v3dcore's (%ld bytes, %lu): v3dcore's "
                   "used", (LONG)sb.used, n);
    return packets_state == 1;
}

/* ---- glDrawArrays ---- */

/* Probe P: the time since the last mark goes to stage k (frame.c reports
 * them): 0 checks and the index range, 1 shaders, 2 textures, 3 uniforms,
 * 4 vertex data, 5 the record, 6 indices, 7 the binning list. */
static void mark(struct OGLES2Context *ctx, int k)
{
    if (ctx->probe & OGLES2_PROBE_PERIODIC) {
        ULONG t = ogles2_clock_us();
        if (k >= 0)
            ctx->stage[k] += (double)(ULONG)(t - ctx->t_stage);
        ctx->t_stage = t;
    }
}

/* An indexed draw's indices, already rebased to the copied vertex range. */
static ULONG index_size(GLenum type)
{
    return type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
}

struct elements
{
    const void *indices;  /* the application's */
    GLenum type;          /* GL_UNSIGNED_BYTE, _SHORT or _INT (OES_element_index_uint) */
    GLsizei count;
    ULONG min;            /* the smallest index: subtracted from each */
};

/* Draws with program p and the context's state; arguments are valid. The
 * vertices [first, first+count) are copied; without `el` they are drawn in
 * order, with it through its indices. */
static void draw_arrays(struct OGLES2Context *ctx, struct OGLES2Program *p, GLenum mode,
                        GLint first, GLsizei count, const struct elements *el)
{
    const struct OGLES2Image *im = p->image;
    const struct OGLES2Variant *cs, *vs, *fs;
    const struct OGLES2Image *fim; /* the fragment shader's image */
    GLenum fsmode;
    struct OGLES2Draw *d;
    V3DContext *v = ctx->v3d;
    struct attr_src src[OGLES2_MAX_VERTEX_ATTRIBS];
    struct tex_states tst;
    ULONG unif[3], code[3], defattr, rec, idx_addr = 0, idx_size = 0;
    int a, nslots = 0, i;

    if (!(d = draw_state(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    mark(ctx, 0);
    /* The program's variants for this primitive mode, as at its last draw
     * (they and their code stay until it is linked again). The fragment
     * shader's is the class the rasterizer sees: triangles in glPolygonMode's
     * GL_LINE or GL_POINT are lines or points to it (Mesa's
     * v3d_rasterizes_lines/points). */
    fsmode = mode >= GL_TRIANGLES && ctx->state.polygon_mode != GL_FILL
                 ? (ctx->state.polygon_mode == GL_POINT ? GL_POINTS : GL_LINES) : mode;
    /* And from the program's 32-bit-sampler variants while a float or depth
     * texture is bound to one of its samplers (program.c, the .v3dw). */
    fim = im;
    if (ogles2_wide_seen && wants_wide(ctx, p)) {
        const struct OGLES2Image *w = ogles2_image_wide((struct OGLES2Image *)im);
        if (w)
            fim = w;
    }
    /* A 4x MSAA default framebuffer with GL_SAMPLE_ALPHA_TO_COVERAGE: the
     * program's alpha-to-coverage variants (the .v3da; Mesa turns alpha into
     * the sample mask in the fragment shader). Not with 32-bit samplers at
     * the same time: no file holds both, and those keep their variants. */
    if (ctx->msaa && !ctx->draw_fb && (ctx->state.caps & OGLES2_CAP_SAMPLE_ALPHA_TO_COVERAGE) && fim == im) {
        const struct OGLES2Image *a = ogles2_image_a2c((struct OGLES2Image *)im);
        if (a)
            fim = a;
    }
    if (p->var_ok && p->var_mode == mode && p->var_fsmode == fsmode && p->var_fim == fim &&
        p->var_nrt == ctx->fb_nrt) {
        cs = p->var[0];
        vs = p->var[1];
        fs = p->var[2];
        code[0] = p->var_code[0];
        code[1] = p->var_code[1];
        code[2] = p->var_code[2];
    } else {
        UBYTE cbufs = (UBYTE)((1 << ctx->fb_nrt) - 1);
        cs = ogles2_image_variant(im, OGLES2_VARIANT_CS, 0, mode);
        vs = ogles2_image_variant(im, OGLES2_VARIANT_VS, 0, mode);
        fs = ogles2_image_variant(fim, OGLES2_VARIANT_FS, cbufs, fsmode);
        if (!cs || !vs || !fs) {
            ogles2_log("glDrawArrays: the program has no shader for this primitive; nothing drawn");
            return;
        }
        if (d->missed != im && (fs->is_points != (fsmode == GL_POINTS) ||
                                fs->is_lines != (fsmode == GL_LINES || fsmode == GL_LINE_LOOP ||
                                                 fsmode == GL_LINE_STRIP))) {
            d->missed = im; /* plan 7.8: the closest variant is used; logged once per program */
            ogles2_log("glDrawArrays: no fragment shader variant for primitive %ld; using the closest",
                       (ULONG)mode);
        }
        if (d->missed_rt != im && (fs->cbufs ? fs->cbufs : 1) != cbufs) {
            d->missed_rt = im; /* logged once per program */
            ogles2_log("glDrawArrays: no fragment shader variant for %ld render targets "
                       "(EXT_draw_buffers): the program draws into the first only", (ULONG)ctx->fb_nrt);
        }
        code[0] = code_address(d, im, cs);
        code[1] = code_address(d, im, vs);
        code[2] = code_address(d, fim, fs);
        if (!code[0] || !code[1] || !code[2]) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        p->var[0] = cs;
        p->var[1] = vs;
        p->var[2] = fs;
        p->var_code[0] = code[0];
        p->var_code[1] = code[1];
        p->var_code[2] = code[2];
        p->var_mode = mode;
        p->var_fsmode = fsmode;
        p->var_fim = fim;
        p->var_nrt = ctx->fb_nrt;
        p->var_ok = TRUE;
    }

    ogles2_job_begin(ctx);

    /* State buffer: uniform streams, vertex data, the record. */
    mark(ctx, 1);
    update_state_vars(ctx, p);
    memset(&tst, 0, sizeof(tst));
    {
        const struct OGLES2Variant *vv[3];
        vv[0] = cs; vv[1] = vs; vv[2] = fs;
        if (!ubo_copies(ctx, p, vv, &tst) || !spill_area(ctx, vv))
            goto nomem;
    }
    if (!texture_states(ctx, p, &tst))
        goto nomem;
    mark(ctx, 2);
    uniform_state(ctx);
    unif[0] = vertex_uniforms(ctx, p, 0, cs, &tst);
    unif[1] = vertex_uniforms(ctx, p, 1, vs, &tst);
    unif[2] = write_uniforms(ctx, p, fs, &tst);
    defattr = default_attribs(ctx, d);
    if (!unif[0] || !unif[1] || !unif[2] || !defattr)
        goto nomem;
    mark(ctx, 3);
    /* One attribute record per vertex input slot, from the generic attribute
     * bound to it (a matrix: one slot and one generic attribute per column). */
    /* Mapped at the program's first draw (attribute locations are fixed
     * at link). */
    if (p->nslots == 0xff) {
        int k, cs;
        for (a = 0; a < im->nattribs; a++)
            for (k = 0; k < im->attribs[a].columns; k++)
                if ((cs = column_slot(&im->attribs[a], k)) >= 0 && cs + 1 > nslots)
                    nslots = cs + 1;
        if (nslots > 16)
            nslots = 16;
        for (i = 0; i < nslots; i++) {
            GLuint g = 0xff;
            for (a = 0; a < im->nattribs; a++)
                for (k = 0; k < im->attribs[a].columns; k++)
                    if (column_slot(&im->attribs[a], k) == (int)i)
                        g = p->attrib_index[a] + k;
            p->slot_attrib[i] = (UBYTE)(g < OGLES2_MAX_VERTEX_ATTRIBS ? g : 0xff);
        }
        p->nslots = (UBYTE)nslots;
    }
    nslots = p->nslots;
    for (i = 0; i < nslots; i++) {
        GLuint g = p->slot_attrib[i];
        BOOL ok;
        if (g >= OGLES2_MAX_VERTEX_ATTRIBS) {
            ok = constant_attrib(ctx, 0, &src[i]); /* a slot no attribute claims */
        } else {
            ok = ctx->array[g].enabled
                     ? copy_array(ctx, &ctx->array[g], first, count, &src[i],
                                  i == 0 && (ctx->probe & OGLES2_PROBE_ADDR) && ctx->probe_addr)
                     : constant_attrib(ctx, g, &src[i]);
        }
        if (!ok)
            goto nomem;
    }
    mark(ctx, 4);
    if (!(rec = shader_record(ctx, p, defattr, code, unif, cs, vs, fs, mode, src, nslots)))
        goto nomem;
    mark(ctx, 5);
    if (el) {
        /* The indices, rebased to the copied range: 8-bit as they are,
         * 16- and 32-bit swapped to little-endian (plan 7.4). */
        ULONG isize = (ULONG)el->count * index_size(el->type), k;
        UBYTE *ib = claim(v, isize + 4, 4);
        if (!ib)
            goto nomem;
        if (el->type == GL_UNSIGNED_INT) {
            swap_indices((ULONG *)ib, el->indices, (ULONG)el->count, el->min);
        } else if (el->type == GL_UNSIGNED_SHORT) {
            const UWORD *in = el->indices;
            for (k = 0; k < (ULONG)el->count; k++) {
                UWORD x = (UWORD)(in[k] - el->min);
                ib[2 * k] = (UBYTE)x;
                ib[2 * k + 1] = (UBYTE)(x >> 8);
            }
        } else {
            const UBYTE *in = el->indices;
            for (k = 0; k < (ULONG)el->count; k++)
                ib[k] = (UBYTE)(in[k] - el->min);
        }
        idx_addr = (ULONG)ib;
        idx_size = isize;
        ctx->n_indices += el->count;
    }

    /* Binning list. */
    mark(ctx, 6);
    v->current_buf = &v->binning_buf[v->build_slot];
    emit_state(ctx, &d->dd, (int)mode, fs);
    if (packets_ok(v)) {
        int itype = !el ? -1
                  : el->type == GL_UNSIGNED_INT ? v3d_INDEX_TYPE_32_BIT
                  : el->type == GL_UNSIGNED_SHORT ? v3d_INDEX_TYPE_16_BIT : v3d_INDEX_TYPE_8_BIT;
        UBYTE *b = v3d_buffer_claim_memory(v->current_buf, el ? 31 : 22);
        if (b)
            draw_packets(b, cs->vcm_cache_size, vs->vcm_cache_size, rec, nslots ? nslots : 1, (UBYTE)mode,
                         itype, idx_addr, idx_size, el ? (ULONG)el->count : (ULONG)count);
        mark(ctx, 7);
        return;
    }
    PointSize(v, 1.0f);
    VCMCacheSize(v, cs->vcm_cache_size, vs->vcm_cache_size); /* Mesa: before every shader state */
    glShaderState(v, rec, nslots ? nslots : 1);
    if (el) {
        /* Mesa's order: INDEX_BUFFER_SETUP, then INDEXED_PRIM_LIST. No
         * primitive restart in GLES2 (plan 7.5). */
        IndexBufferSetup(v, idx_addr, idx_size);
        IndexedPrimList(v, (UBYTE)mode, el->type == GL_UNSIGNED_INT     ? v3d_INDEX_TYPE_32_BIT
                                        : el->type == GL_UNSIGNED_SHORT ? v3d_INDEX_TYPE_16_BIT
                                                                        : v3d_INDEX_TYPE_8_BIT,
                        (ULONG)el->count, FALSE, 0);
    } else {
        VertexArrayPrims(v, (UBYTE)mode, (ULONG)count, 0); /* the copies start at `first` */
    }
    mark(ctx, 7);
    return;

nomem:
    ogles2_log("glDrawArrays: out of memory for the draw's GPU data; frame dropped");
    v->frame_corrupted = TRUE;
    ogles2_error(ctx, GL_OUT_OF_MEMORY);
}

/* The vertex input slot of column k of an attribute (a matrix's columns are
 * generic attributes of their own), -1 if the shader does not read it. */
static int column_slot(const struct OGLES2ImageAttrib *at, int k)
{
    int j, n = 0;
    if (at->slot == 0xff)
        return -1;
    if (!at->colmask) /* every column, from the slot on */
        return at->slot + k;
    if (!(at->colmask & (1 << k)))
        return -1;
    for (j = 0; j < k; j++)
        if (at->colmask & (1 << j))
            n++;
    return at->slot + n;
}

/* Every enabled array the program reads has data, and a buffer's holds
 * vertices [0, end) (GLES2 leaves reading past it undefined: not drawn). */
static BOOL arrays_ok(struct OGLES2Context *ctx, const struct OGLES2Program *p, ULONG end,
                      const char *fn)
{
    int a;
    for (a = 0; a < p->image->nattribs; a++) {
        const struct OGLES2Array *arr = &ctx->array[p->attrib_index[a]];
        if (p->image->attribs[a].slot == 0xff || !arr->enabled)
            continue;
        if (arr->buffer) {
            ULONG es = (ULONG)(type_size(arr->type) * arr->size);
            ULONG st = arr->stride ? (ULONG)arr->stride : es;
            ULONG off = (ULONG)arr->pointer, size = ogles2_buffer_size(arr->buffer);
            if (off + es > size || (size - off - es) / st + 1 < end) {
                ogles2_log("%s: attribute %ld reads past the end of buffer %ld; nothing drawn",
                           (ULONG)fn, (ULONG)p->attrib_index[a], (ULONG)ogles2_buffer_name(arr->buffer));
                return FALSE;
            }
        } else if (!arr->pointer) {
            ogles2_log("%s: attribute %ld is enabled without an array; nothing drawn", (ULONG)fn,
                       (ULONG)p->attrib_index[a]);
            return FALSE;
        }
    }
    return TRUE;
}

void ogles2_glDrawArrays(struct OGLES2Context *ctx, GLenum mode, GLint first, GLsizei count)
{
    ULONG t0 = ogles2_clock_us();
    struct OGLES2Program *p;

    if (!ogles2_context_valid(ctx))
        return;
    mark(ctx, -1);
    if (mode > GL_TRIANGLE_FAN) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (first < 0 || count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ogles2_fb_ready(ctx)) /* GL_INVALID_FRAMEBUFFER_OPERATION */
        return;
    p = ctx->program;
    if (!p || !ctx->v3d || count == 0) /* no program: undefined; nothing is drawn */
        return;
    if (!arrays_ok(ctx, p, (ULONG)first + (ULONG)count, "glDrawArrays"))
        return;
    draw_arrays(ctx, p, mode, first, count, 0);
    ctx->n_draws++;
    ctx->sum_draw += (double)(ULONG)(ogles2_clock_us() - t0);
}

/* glDrawElements, and with a base vertex added to every index
 * (OES_draw_elements_base_vertex): the copied range moves by basevertex,
 * the rebased indices stay as they are. With `range` (glDrawRangeElements)
 * the program has said which indices it uses, range[0] to range[1]: they
 * are not scanned. */
static void draw_elements(struct OGLES2Context *ctx, GLenum mode, GLsizei count, GLenum type,
                          const void *indices, GLint basevertex, const ULONG *range, const char *fn)
{
    struct OGLES2Program *p;
    struct elements el;
    ULONG lo = ~0UL, hi = 0, k, t0 = ogles2_clock_us();

    if (!ogles2_context_valid(ctx))
        return;
    mark(ctx, -1);
    if (mode > GL_TRIANGLE_FAN ||
        (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ogles2_fb_ready(ctx))
        return;
    p = ctx->program;
    if (!p || !ctx->v3d || count == 0)
        return;
    {
        /* With an element buffer bound, `indices` is the offset into it. */
        BOOL bad;
        const void *in = ogles2_buffer_indices(ctx, (ULONG)indices,
                                               (ULONG)count * index_size(type), &bad);
        if (bad) {
            ogles2_log("%s: the indices lie outside the element buffer; nothing drawn", (ULONG)fn);
            return;
        }
        if (in)
            indices = in;
        else if (!indices)
            return;
    }
    /* Mesa's state tracker: copy only the range the indices use. Without
     * branches (Emu68 runs a loop with two of them a pass at a third of the
     * speed): d's sign bit, spread by the arithmetic shift, picks it. Indices
     * past 2^31 would not fit the vertex arrays anyway. */
    if (range) {
        /* About 25 ns an index saved (Quake III through gl4es: 3.5 us a
         * draw). An index outside the range is undefined (GLES 3.0 2.9.5):
         * the GPU reads other vertices. */
        if (range[1] > 0x7FFFFFFFUL) {
            ogles2_log("%s: an end of 2^31 or more; nothing drawn", (ULONG)fn);
            return;
        }
        lo = range[0];
        hi = range[1];
    } else if (type == GL_UNSIGNED_INT) {
        const ULONG *in = indices;
        LONG l = 0x7FFFFFFF, h = 0, d;
        ULONG any = 0;
        for (k = 0; k < (ULONG)count; k++) {
            LONG x = (LONG)(in[k] & 0x7FFFFFFFUL);
            any |= in[k];
            d = x - l;
            l += d & (d >> 31);
            d = h - x;
            h -= d & (d >> 31);
        }
        if (any & 0x80000000UL) {
            ogles2_log("%s: an index of 2^31 or more; nothing drawn", (ULONG)fn);
            return;
        }
        lo = (ULONG)l;
        hi = (ULONG)h;
    } else if (type == GL_UNSIGNED_SHORT) {
        const UWORD *in = indices;
        LONG l = 0x7FFFFFFF, h = 0, d;
        for (k = 0; k < (ULONG)count; k++) {
            LONG x = in[k];
            d = x - l;
            l += d & (d >> 31);
            d = h - x;
            h -= d & (d >> 31);
        }
        lo = (ULONG)l;
        hi = (ULONG)h;
    } else {
        const UBYTE *in = indices;
        LONG l = 0x7FFFFFFF, h = 0, d;
        for (k = 0; k < (ULONG)count; k++) {
            LONG x = in[k];
            d = x - l;
            l += d & (d >> 31);
            d = h - x;
            h -= d & (d >> 31);
        }
        lo = (ULONG)l;
        hi = (ULONG)h;
    }
    if ((LONG)lo + basevertex < 0) {
        ogles2_log("%s: index %ld + base vertex %ld is negative; nothing drawn", (ULONG)fn, (LONG)lo,
                   (LONG)basevertex);
        return;
    }
    lo += (ULONG)basevertex;
    hi += (ULONG)basevertex;
    if (!arrays_ok(ctx, p, hi + 1, fn))
        return;
    el.indices = indices;
    el.type = type;
    el.count = count;
    el.min = lo - (ULONG)basevertex; /* the indices' own smallest */
    draw_arrays(ctx, p, mode, (GLint)lo, (GLsizei)(hi - lo + 1), &el);
    ctx->n_draws++;
    ctx->sum_draw += (double)(ULONG)(ogles2_clock_us() - t0);
}

void ogles2_glDrawElements(struct OGLES2Context *ctx, GLenum mode, GLsizei count, GLenum type,
                           const void *indices)
{
    draw_elements(ctx, mode, count, type, indices, 0, 0, "glDrawElements");
}

void ogles2_glDrawElementsBaseVertexOES(struct OGLES2Context *ctx, GLenum mode, GLsizei count,
                                        GLenum type, const void *indices, GLint basevertex)
{
    draw_elements(ctx, mode, count, type, indices, basevertex, 0, "glDrawElementsBaseVertexOES");
}

/* GLES 3.0's glDrawRangeElements, offered to GLES2 programs through
 * aglGetProcAddress (ports/gl4es passes a locked array range): the indices
 * lie in [start, end], so ogles2 does not scan them for their range. */
void ogles2_glDrawRangeElements(struct OGLES2Context *ctx, GLenum mode, GLuint start, GLuint end,
                                GLsizei count, GLenum type, const void *indices)
{
    ULONG range[2];
    if (!ogles2_context_valid(ctx))
        return;
    if (end < start) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    range[0] = start;
    range[1] = end;
    draw_elements(ctx, mode, count, type, indices, 0, range, "glDrawRangeElements");
}

#include "clear_mrt_v3dp.h"

/* The program a clear draws into several render targets with: the clear
 * colour (the last generic attribute, constant) in every output, position
 * from the last but one; tools/clear_mrt. 0: none (logged once), and the
 * built-in one clears the first target only. */
static struct OGLES2Program *mrt_program(struct OGLES2Draw *d)
{
    struct OGLES2Program *p = &d->mrt_quad;
    UWORD a;
    if (!d->mrt_ready && !d->mrt_failed) {
        char why[96];
        if (!(d->mrt_image = ogles2_v3dp_load(clear_mrt_v3dp, sizeof(clear_mrt_v3dp), why, sizeof(why))) ||
            !ogles2_program_image(p, d->mrt_image)) {
            d->mrt_failed = TRUE;
            ogles2_log("glClear: no program for several render targets (%s): the first is cleared only",
                       (ULONG)(d->mrt_image ? "out of memory" : why));
            return 0;
        }
        d->mrt_ready = TRUE;
    }
    if (!d->mrt_ready)
        return 0;
    for (a = 0; a < d->mrt_image->nattribs; a++)
        p->attrib_index[a] = d->mrt_image->attribs[a].name[2] == 'p' /* a_position */
                                 ? OGLES2_MAX_VERTEX_ATTRIBS - 2 : OGLES2_MAX_VERTEX_ATTRIBS - 1;
    return p;
}

#include "copy_tex_v3dp.h"

/* The program glCopyTex* draws with on the GPU: unit 0's 2D texture over a
 * quad, position from the last but one generic attribute, texture
 * coordinates from the last; tools/copy_tex. 0: none (logged once), and the
 * copies go through the CPU. */
static struct OGLES2Program *copy_program(struct OGLES2Draw *d)
{
    struct OGLES2Program *p = &d->copy_prog;
    UWORD a;
    if (!d->copy_ready && !d->copy_failed) {
        char why[96];
        if (!(d->copy_image = ogles2_v3dp_load(copy_tex_v3dp, sizeof(copy_tex_v3dp), why, sizeof(why))) ||
            !ogles2_program_image(p, d->copy_image)) {
            d->copy_failed = TRUE;
            ogles2_log("glCopyTexSubImage2D: no program for copies on the GPU (%s): the CPU copies",
                       (ULONG)(d->copy_image ? "out of memory" : why));
            return 0;
        }
        d->copy_ready = TRUE;
    }
    if (!d->copy_ready)
        return 0;
    for (a = 0; a < d->copy_image->nattribs; a++)
        p->attrib_index[a] = d->copy_image->attribs[a].name[2] == 'p' /* a_position */
                                 ? OGLES2_MAX_VERTEX_ATTRIBS - 2 : OGLES2_MAX_VERTEX_ATTRIBS - 1;
    return p;
}

/* glCopyTexImage2D and glCopyTexSubImage2D on the GPU (copy.c): texture
 * src, bound to unit 0 for the draw, over the window rectangle (x, y, w, h)
 * of the bound framebuffer (the copy's target), texture coordinates tc[0]
 * to tc[2] left to right and tc[1] to tc[3] bottom to top. None of the
 * application's state applies: no test, blending, dithering or mask, as a
 * copy writes. The application's state is restored afterwards. FALSE:
 * nothing drawn (no program). */
BOOL ogles2_draw_copy(struct OGLES2Context *ctx, struct OGLES2Texture *src, const GLfloat *tc, GLint x, GLint y,
                      GLsizei w, GLsizei h)
{
    struct OGLES2State saved = ctx->state, *s = &ctx->state;
    struct OGLES2Array saved_pos = ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2];
    struct OGLES2Array saved_tc = ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 1];
    struct OGLES2Program *saved_prog = ctx->program, *p;
    struct OGLES2Texture *saved_tex;
    struct OGLES2Draw *d;
    static const GLfloat pos[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLfloat uv[8];
    GLint saved_vp[4];
    int k;

    if (!(d = draw_state(ctx)) || !(p = copy_program(d)))
        return FALSE;
    for (k = 0; k < 4; k++)
        saved_vp[k] = ctx->viewport[k];
    uv[0] = tc[0]; uv[1] = tc[1]; uv[2] = tc[2]; uv[3] = tc[1];
    uv[4] = tc[0]; uv[5] = tc[3]; uv[6] = tc[2]; uv[7] = tc[3];
    for (k = 0; k < 2; k++) {
        struct OGLES2Array *a = &ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2 + k];
        a->enabled = GL_TRUE;
        a->size = 2;
        a->type = GL_FLOAT;
        a->normalized = GL_FALSE;
        a->stride = 0;
        a->pointer = k ? (const void *)uv : (const void *)pos;
        a->buffer = 0;
    }
    s->caps = 0;
    s->polygon_mode = GL_FILL;
    s->color_mask[0] = s->color_mask[1] = s->color_mask[2] = s->color_mask[3] = GL_TRUE;
    s->depth_range[0] = 0.0f;
    s->depth_range[1] = 1.0f;
    s->clip_origin = GL_LOWER_LEFT_EXT;
    s->clip_depth = GL_NEGATIVE_ONE_TO_ONE_EXT;
    ctx->viewport[0] = x;
    ctx->viewport[1] = y;
    ctx->viewport[2] = w;
    ctx->viewport[3] = h;
    saved_tex = ogles2_texture_unit0(ctx, src);

    ogles2_job_begin(ctx);
    ctx->state_ver++; /* the override above */
    mark(ctx, -1);
    draw_arrays(ctx, p, GL_TRIANGLE_STRIP, 0, 4, 0);

    ogles2_texture_unit0(ctx, saved_tex);
    ctx->state = saved;
    ctx->state_ver++;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2] = saved_pos;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 1] = saved_tc;
    ctx->program = saved_prog;
    for (k = 0; k < 4; k++)
        ctx->viewport[k] = saved_vp[k];
    return TRUE;
}

/* ---- clears that the tile buffer cannot do ----
 * A clear confined by the scissor box or by write masks, or one after draws
 * in the same job (the tile buffer clears before any of them), is drawn as a
 * rectangle (plan 7.7; PiStorm3D's gl_ClearViaQuad, Mesa's blitter clear):
 * the built-in program, an identity matrix, the clear colour as a constant
 * attribute, z at the clear depth, and a state that writes exactly what
 * glClear writes. The application's state is restored afterwards. */
void ogles2_draw_clear(struct OGLES2Context *ctx, GLbitfield mask)
{
    static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    struct OGLES2State saved = ctx->state, *s = &ctx->state;
    struct OGLES2Array saved_pos = ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2];
    struct OGLES2Program *saved_prog = ctx->program, *quad;
    struct OGLES2Draw *d;
    GLint saved_vp[4];
    GLfloat pos[12], z;
    int k;

    if ((mask & GL_DEPTH_BUFFER_BIT) && (!ctx->fb_depth_bits || !s->depth_mask))
        mask &= ~(GLbitfield)GL_DEPTH_BUFFER_BIT;
    if ((mask & GL_STENCIL_BUFFER_BIT) && !ctx->fb_stencil_bits)
        mask &= ~(GLbitfield)GL_STENCIL_BUFFER_BIT;
    if (!mask)
        return;
    for (k = 0; k < 4; k++)
        saved_vp[k] = ctx->viewport[k];

    /* The program: the built-in image, an identity matrix, position from
     * the last-but-one generic attribute, colour from the last (constant). */
    if (!(d = draw_state(ctx)))
        return;
    if (!d->quad_ready) {
        if (!ogles2_program_builtin(&d->quad))
            return;
        d->quad_ready = TRUE;
        for (k = 0; k < 16; k++)
            ogles2_cbuf_set(&d->quad, 0, (ULONG)k, float_bits(identity[k]));
    }
    quad = &d->quad;
    quad->attrib_index[0] = OGLES2_MAX_VERTEX_ATTRIBS - 2;
    quad->attrib_index[1] = OGLES2_MAX_VERTEX_ATTRIBS - 1;
    /* EXT_draw_buffers: GLES2 clears every draw buffer. */
    if (ctx->fb_nrt > 1 && (mask & GL_COLOR_BUFFER_BIT)) {
        struct OGLES2Program *m = mrt_program(d);
        if (m)
            quad = m;
    }

    z = s->clear_depth * 2.0f - 1.0f; /* lands on clear_depth with the range [0, 1] */
    pos[0] = -1; pos[1] = -1;  pos[3] = 1; pos[4] = -1;
    pos[6] = -1; pos[7] = 1;   pos[9] = 1; pos[10] = 1;
    pos[2] = pos[5] = pos[8] = pos[11] = z;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].enabled = GL_TRUE;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].size = 3;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].type = GL_FLOAT;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].normalized = GL_FALSE;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].stride = 0;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].pointer = pos;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2].buffer = 0;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 1].enabled = GL_FALSE;
    for (k = 0; k < 4; k++)
        s->attrib[OGLES2_MAX_VERTEX_ATTRIBS - 1][k] = s->clear_color[k];

    /* The state: only the scissor test stays the application's (the
     * rectangle is filled whatever glPolygonMode says). */
    s->caps &= OGLES2_CAP_SCISSOR_TEST;
    s->polygon_mode = GL_FILL;
    if (!(mask & GL_COLOR_BUFFER_BIT))
        s->color_mask[0] = s->color_mask[1] = s->color_mask[2] = s->color_mask[3] = GL_FALSE;
    if (mask & GL_DEPTH_BUFFER_BIT) {
        s->caps |= OGLES2_CAP_DEPTH_TEST;
        s->depth_func = GL_ALWAYS;
    }
    if (mask & GL_STENCIL_BUFFER_BIT) {
        s->caps |= OGLES2_CAP_STENCIL_TEST;
        for (k = 0; k < 2; k++) {
            s->stencil_func[k] = GL_ALWAYS;
            s->stencil_ref[k] = s->clear_stencil & 0xff;
            s->stencil_fail[k] = s->stencil_zfail[k] = s->stencil_zpass[k] = GL_REPLACE;
            s->stencil_write_mask[k] = saved.stencil_write_mask[0]; /* glClear uses the front mask */
        }
    }
    s->depth_range[0] = 0.0f;
    s->depth_range[1] = 1.0f;
    s->clip_origin = GL_LOWER_LEFT_EXT;
    s->clip_depth = GL_NEGATIVE_ONE_TO_ONE_EXT;
    ctx->viewport[0] = ctx->viewport[1] = 0;
    ctx->viewport[2] = (GLint)ctx->fb_w;
    ctx->viewport[3] = (GLint)ctx->fb_h;

    ogles2_job_begin(ctx);
    ctx->state_ver++; /* the override above */
    mark(ctx, -1); /* probe P: the time before is not the draw's */
    draw_arrays(ctx, quad, GL_TRIANGLE_STRIP, 0, 4, 0);

    ctx->state = saved;
    ctx->state_ver++;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2] = saved_pos;
    ctx->program = saved_prog;
    for (k = 0; k < 4; k++)
        ctx->viewport[k] = saved_vp[k];
}
