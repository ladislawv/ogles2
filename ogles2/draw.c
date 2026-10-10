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
    BOOL cfg, cw, vp, blend, cwm, lw, st;
    UBYTE effp, erfp, cp, edo, dtf, zue, eze, ezue, be, se;
    UBYTE stencil[2][5];  /* the STENCIL_CFG bytes last sent, front and back */
    UWORD cw_l, cw_t, cw_w, cw_h;
    UWORD vp_w, vp_h, vp_cx2, vp_cy2;
    UBYTE b_src, b_dst, b_eq, b_asrc, b_adst, b_aeq;
    UWORD b_color[4];
    ULONG cwm_mask;
    float lw_value, do_factor, do_units;
    float vp_zs, vp_zo;
    BOOL do_valid;
    int prim;
};

struct OGLES2Draw
{
    v3d_mem builtin_code;          /* the built-in image's code, uploaded at first use */
    struct OGLES2Program quad;     /* clears drawn as rectangles (built-in image) */
    BOOL quad_ready;
    struct dedup dd;
    v3d_u32 defattr_gen;           /* the default attribute block of this control list */
    ULONG defattr;
    const struct OGLES2Image *missed; /* the last image a variant miss was logged for */
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
    if (d->quad_ready)
        ogles2_program_builtin_free(&d->quad);
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
        GLfloat *v = (GLfloat *)p->cbuf[sv->stage] + sv->offset;
        switch (sv->state) {
        case OGLES2_STATE_DEPTH_RANGE:
            v[0] = dr[0];
            v[1] = dr[1];
            v[2] = dr[1] - dr[0];
            v[3] = 1.0f;
            break;
        case OGLES2_STATE_FB_WPOS_Y_TRANSFORM: /* flipped: (-1, height, 1, 0) */
            v[0] = -1.0f;
            v[1] = (GLfloat)ctx->height;
            v[2] = 1.0f;
            v[3] = 0.0f;
            break;
        case OGLES2_STATE_FB_PNTC_Y_TRANSFORM: /* upper-left origin on a flipped buffer */
            v[0] = -1.0f;
            v[1] = 1.0f;
            v[2] = 0.0f;
            v[3] = 0.0f;
            break;
        }
    }
}

/* Texture and sampler state per texture index of each stage (the TMU
 * recipes' unit field), [0] vertex, [1] fragment; 0 where unused. */
struct tex_states
{
    ULONG tex[2][OGLES2_MAX_TEXTURE_UNITS], smp[2][OGLES2_MAX_TEXTURE_UNITS];
    ULONG ubo[2];   /* little-endian copy of each stage's constant buffer, if read */
};

/* QUNIFORM_UBO_ADDR (indirectly indexed uniform arrays, plan 7.8): the
 * shader reads its constant buffer through the TMU, so a stage whose recipe
 * asks for it gets a little-endian copy in the state buffer. Made before any
 * uniform stream (a claim must not happen while another is half filled). */
static BOOL ubo_copies(struct OGLES2Context *ctx, const struct OGLES2Program *p,
                       const struct OGLES2Variant *const v[3], struct tex_states *ts)
{
    int k, i, si;
    for (k = 0; k < 3; k++) {
        si = v[k]->stage == OGLES2_VARIANT_FS ? 1 : 0;
        if (ts->ubo[si])
            continue;
        for (i = 0; i < v[k]->nrecipe; i++)
            if (v[k]->rtype[i] == OGLES2_U_UBO_ADDR)
                break;
        if (i < v[k]->nrecipe) {
            UWORD n = p->image->cbuf_words[si], w;
            ULONG *c = claim(ctx->v3d, n * 4UL + 16, 16);
            if (!c)
                return FALSE;
            for (w = 0; w < n; w++)
                c[w] = le32(p->cbuf[si][w]);
            ts->ubo[si] = (ULONG)c;
        }
    }
    return TRUE;
}

/* Every sampler uniform of the program: its texture index in each stage
 * that uses it reads the texture unit glUniform1i gave it. */
static BOOL texture_states(struct OGLES2Context *ctx, const struct OGLES2Program *p,
                           struct tex_states *ts)
{
    const struct OGLES2Image *im = p->image;
    UWORD u;
    int st;
    for (u = 0; u < im->nuniforms; u++) {
        const struct OGLES2ImageUniform *un = &im->uniforms[u];
        if (!(un->flags & OGLES2_UNIFORM_SAMPLER))
            continue;
        for (st = 0; st < 2; st++) {
            ULONG idx = un->sampler[st];
            if (idx >= OGLES2_MAX_TEXTURE_UNITS)
                continue;
            if (!ogles2_texture_state(ctx, p->sampler_unit[u], un->type == GL_SAMPLER_CUBE,
                                      &ts->tex[st][idx], &ts->smp[st][idx]))
                return FALSE;
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
    const ULONG *cbuf = p->cbuf[si];
    const struct OGLES2State *s = &ctx->state;
    const GLfloat *dr = s->depth_range;
    ULONG *u;
    int i;
    if (!(u = claim(ctx->v3d, (var->nrecipe + 1) * 4UL, 4)))
        return 0;
    for (i = 0; i < var->nrecipe; i++) {
        ULONG d = var->rdata[i], w;
        switch (var->rtype[i]) {
        case OGLES2_U_CONSTANT:          w = d; break;
        case OGLES2_U_UNIFORM:           w = cbuf[d]; break;
        /* The default framebuffer is drawn upside down (plan 7.11): a
         * negative Y scale, as Mesa's state tracker gives a window. */
        case OGLES2_U_VIEWPORT_X_SCALE:  w = float_bits(ctx->viewport[2] * 0.5f * 256.0f); break;
        case OGLES2_U_VIEWPORT_Y_SCALE:  w = float_bits(ctx->viewport[3] * -0.5f * 256.0f); break;
        case OGLES2_U_VIEWPORT_Z_SCALE:  w = float_bits((dr[1] - dr[0]) * 0.5f); break;
        case OGLES2_U_VIEWPORT_Z_OFFSET: w = float_bits((dr[1] + dr[0]) * 0.5f); break;
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
            w = tst->tex[si][(d >> 24) & (OGLES2_MAX_TEXTURE_UNITS - 1)] | (d & 0xffffff);
            break;
        case OGLES2_U_TMU_CONFIG_P1:
            w = tst->smp[si][(d >> 24) & (OGLES2_MAX_TEXTURE_UNITS - 1)] | (d & 0xffffff);
            break;
        case OGLES2_U_UBO_ADDR:          w = tst->ubo[si] + (d & 0xffffff); break;
        case OGLES2_U_PISTORM_VP_WIDTH:  w = float_bits((GLfloat)ctx->viewport[2]); break;
        case OGLES2_U_PISTORM_VP_HEIGHT: w = float_bits((GLfloat)ctx->viewport[3]); break;
        default:                         w = 0; break; /* the loader refuses the rest */
        }
        u[i] = le32(w);
    }
    u[var->nrecipe] = 0;
    return (ULONG)u;
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

/* `count` elements of `n` components of `type` into the GPU's byte order. */
static void convert_elems(UBYTE *dst, ULONG dst_stride, const UBYTE *src, ULONG src_stride,
                          ULONG count, GLint n, GLenum type)
{
    ULONG i;
    int k;
    switch (type) {
    case GL_FLOAT:
    case GL_FIXED:
        for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
            for (k = 0; k < n; k++) {
                ULONG w;
                memcpy(&w, src + k * 4, 4);
                ((ULONG *)dst)[k] = le32(w);
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
    UBYTE *dst;

    out->vsize = n == 4 ? v3d_VEC_4 : (UBYTE)n;
    out->normalized = a->normalized && a->type != GL_FLOAT && a->type != GL_FIXED;
    out->is_signed = a->type == GL_BYTE || a->type == GL_SHORT;
    /* GL_FIXED is read by the attribute record's fixed type, which is GL's
     * 16.16 (M1 probe F on the A1200, plan 7.5): a byte swap only. */
    out->vtype = a->type == GL_FIXED ? v3d_ATTRIBUTE_FIXED
               : a->type == GL_FLOAT ? v3d_ATTRIBUTE_FLOAT
               : es == 2 ? v3d_ATTRIBUTE_SHORT : v3d_ATTRIBUTE_BYTE;
    if (a->buffer) {
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
    convert_elems(dst, dst_stride, (const UBYTE *)a->pointer + (ULONG)first * src_stride, src_stride,
                  (ULONG)count, n, a->type);
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

/* The GL shader state record and its attribute records, one per vertex
 * input slot (Mesa's emit_shader_state_record and v3d_emit_gl_shader_state,
 * V3D 4.2, no geometry shader); the record's address. */
static ULONG shader_record(struct OGLES2Context *ctx, ULONG defattr, const ULONG code[3],
                           const ULONG unif[3], const struct OGLES2Variant *cs,
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
    BOOL cs_loaded_any = FALSE;
    BOOL cs_builtins = cs->uses_iid || cs->uses_vid;
    int i;

    if (!r)
        return 0;
    memset(r, 0, size);
    r->enable_clipping = TRUE;
    r->point_size_in_shaded_vertex_data = mode == GL_POINTS && vs->per_vertex_point_size;
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
    r->address_of_default_attribute_values = le32(defattr);

    r->fragment_shader_code_address_rshift_3 = code[2] >> 3;
    r->fragment_shader_uniforms_address = le32(unif[2]);
    r->fragment_shader_4_way_threadable = fs->threads == 4;
    r->fragment_shader_start_in_final_thread_section = fs->single_seg;
    r->fragment_shader_propagate_nans = TRUE;

    r->vertex_shader_code_address_rshift_3 = code[1] >> 3;
    r->vertex_shader_uniforms_address = le32(unif[1]);
    r->vertex_shader_4_way_threadable = vs->threads == 4;
    r->vertex_shader_start_in_final_thread_section = vs->single_seg;
    r->vertex_shader_propagate_nans = TRUE;

    r->coordinate_shader_code_address_rshift_3 = code[0] >> 3;
    r->coordinate_shader_uniforms_address = le32(unif[0]);
    r->coordinate_shader_4_way_threadable = cs->threads == 4;
    r->coordinate_shader_start_in_final_thread_section = cs->single_seg;
    r->coordinate_shader_propagate_nans = TRUE;

    /* The bitfield words holding the code addresses, as PiStorm3D swaps them. */
    swivel = (ULONG *)r;
    swivel[3] = le32(swivel[3]);
    swivel[5] = le32(swivel[5]);
    swivel[7] = le32(swivel[7]);

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
         : e == GL_FUNC_REVERSE_SUBTRACT ? V3D_BLEND_MODE_RSUB : V3D_BLEND_MODE_ADD;
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
    BOOL depth_on = (s->caps & OGLES2_CAP_DEPTH_TEST) && ctx->depth_bits;
    BOOL stencil_on = (s->caps & OGLES2_CAP_STENCIL_TEST) && ctx->stencil_bits;
    UBYTE effp, erfp, cp, edo, dtf, zue, eze, ezue, be, se;
    LONG fh = (LONG)ctx->height;
    LONG vx = ctx->viewport[0], vy = ctx->viewport[1], vw = ctx->viewport[2], vh = ctx->viewport[3];
    LONG l, t, r, b;
    UWORD vp_cx2, vp_cy2;
    ULONG cwm;
    float zs = (s->depth_range[1] - s->depth_range[0]) * 0.5f;
    float zo = (s->depth_range[1] + s->depth_range[0]) * 0.5f;
    int k;

    if (dd->gen != g_v3d_cl_generation) { /* a new control list: emit everything */
        memset(dd, 0, sizeof(*dd));
        dd->gen = g_v3d_cl_generation;
        dd->prim = -1;
    }

    /* CLIP_WINDOW: viewport, and scissor box if enabled, in the top-down
     * frame (GL's y is from the bottom), clamped to the framebuffer. */
    l = vx;
    r = vx + vw;
    t = fh - (vy + vh);
    b = fh - vy;
    if (s->caps & OGLES2_CAP_SCISSOR_TEST) {
        LONG sl = ctx->scissor[0], sr = sl + ctx->scissor[2];
        LONG st = fh - (ctx->scissor[1] + ctx->scissor[3]), sb = fh - ctx->scissor[1];
        if (sl > l) l = sl;
        if (sr < r) r = sr;
        if (st > t) t = st;
        if (sb < b) b = sb;
    }
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (LONG)ctx->width) r = (LONG)ctx->width;
    if (b > fh) b = fh;
    if (r < l) r = l;
    if (b < t) b = t;
    if (!dd->cw || dd->cw_l != l || dd->cw_t != t || dd->cw_w != r - l || dd->cw_h != b - t) {
        ClipWindow(v, (UWORD)l, (UWORD)t, (UWORD)(r - l), (UWORD)(b - t));
        dd->cw = TRUE;
        dd->cw_l = (UWORD)l; dd->cw_t = (UWORD)t; dd->cw_w = (UWORD)(r - l); dd->cw_h = (UWORD)(b - t);
    }

    /* CFG_BITS. Culling as Mesa's v3dx_emit.c; the winding flag set for
     * counter-clockwise as PiStorm3D's (its shaders flip y the same way). */
    effp = erfp = TRUE;
    if (s->caps & OGLES2_CAP_CULL_FACE) {
        if (s->cull_face_mode != GL_BACK) effp = FALSE;  /* FRONT or FRONT_AND_BACK */
        if (s->cull_face_mode != GL_FRONT) erfp = FALSE; /* BACK or FRONT_AND_BACK */
    }
    cp = s->front_face == GL_CCW;
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
    if (!dd->cfg || dd->effp != effp || dd->erfp != erfp || dd->cp != cp || dd->edo != edo ||
        dd->dtf != dtf || dd->zue != zue || dd->eze != eze || dd->ezue != ezue || dd->be != be ||
        dd->se != se) {
        CfgBits(v, effp, erfp, cp, edo, V3D_LINE_RASTERIZATION_DIAMOND_EXIT, 0, FALSE, dtf, zue, eze,
                ezue, se, be, FALSE, FALSE);
        dd->cfg = TRUE;
        dd->effp = effp; dd->erfp = erfp; dd->cp = cp; dd->edo = edo; dd->dtf = dtf;
        dd->zue = zue; dd->eze = eze; dd->ezue = ezue; dd->be = be; dd->se = se;
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
    if (edo && (!dd->do_valid || dd->do_factor != s->polygon_offset_factor ||
                dd->do_units != s->polygon_offset_units)) {
        DepthOffset(v, s->polygon_offset_factor, s->polygon_offset_units);
        dd->do_valid = TRUE;
        dd->do_factor = s->polygon_offset_factor;
        dd->do_units = s->polygon_offset_units;
    }
    /* POINT_SIZE on every draw: Mesa's V3D 4.2 binner erratum workaround. */
    PointSize(v, 1.0f);
    if (!dd->lw || dd->lw_value != s->line_width) {
        LineWidth(v, s->line_width);
        dd->lw = TRUE;
        dd->lw_value = s->line_width;
    }

    /* The viewport group (CLIPPER_XY_SCALING, the Z packets, VIEWPORT_OFFSET),
     * as Mesa's v3dx_emit.c: the Z scale and offset from the depth range. */
    vp_cx2 = (UWORD)(2 * vx + vw);              /* twice the centre, from the left */
    vp_cy2 = (UWORD)(2 * (fh - vy) - vh);       /* twice the centre, from the top */
    if (!dd->vp || dd->vp_w != vw || dd->vp_h != vh || dd->vp_cx2 != vp_cx2 || dd->vp_cy2 != vp_cy2 ||
        dd->vp_zs != zs || dd->vp_zo != zo) {
        ClipperXYScaling(v, (UWORD)vw, (UWORD)vh);
        ClipperZScaleAndOffset(v, zs, zo);
        ClipperZMinMaxClippingPlanes(v, zo - zs < zo + zs ? zo - zs : zo + zs,
                                     zo - zs < zo + zs ? zo + zs : zo - zs);
        ViewportOffset(v, vp_cx2, 0, vp_cy2, 0);
        dd->vp = TRUE;
        dd->vp_w = (UWORD)vw; dd->vp_h = (UWORD)vh; dd->vp_cx2 = vp_cx2; dd->vp_cy2 = vp_cy2;
        dd->vp_zs = zs; dd->vp_zo = zo;
    }

    /* Blending: BLEND_ENABLES, BLEND_CFG (all four render targets, as Mesa
     * and PiStorm3D), BLEND_CONSTANT_COLOR. */
    if (be) {
        UBYTE src = blend_factor(s->blend_src_rgb), dst = blend_factor(s->blend_dst_rgb);
        UBYTE asrc = blend_factor(s->blend_src_alpha), adst = blend_factor(s->blend_dst_alpha);
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

    /* COLOR_WRITE_MASKS: a set bit disables a channel (R, G, B, A = bits 0-3). */
    cwm = (s->color_mask[0] ? 0 : 1) | (s->color_mask[1] ? 0 : 2) |
          (s->color_mask[2] ? 0 : 4) | (s->color_mask[3] ? 0 : 8);
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
        SampleState(v, 1, 0xF); /* coverage 1.0 (the emitter converts it to a float) */
        v->draw_state_configured = TRUE;
        dd->prim = prim;
    } else if (dd->prim != prim) { /* Mesa: on every primitive-type change */
        TransformFeedbackSpecs(v, FALSE, 0);
        dd->prim = prim;
    }
}

/* ---- glDrawArrays ---- */

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
    cs = ogles2_image_variant(im, OGLES2_VARIANT_CS, mode);
    vs = ogles2_image_variant(im, OGLES2_VARIANT_VS, mode);
    fs = ogles2_image_variant(im, OGLES2_VARIANT_FS, mode);
    if (!cs || !vs || !fs) {
        ogles2_log("glDrawArrays: the program has no shader for this primitive; nothing drawn");
        return;
    }
    if (d->missed != im && (fs->is_points != (mode == GL_POINTS) ||
                            fs->is_lines != (mode == GL_LINES || mode == GL_LINE_LOOP ||
                                             mode == GL_LINE_STRIP))) {
        d->missed = im; /* plan 7.8: the closest variant is used; logged once per program */
        ogles2_log("glDrawArrays: no fragment shader variant for primitive %ld; using the closest",
                   (ULONG)mode);
    }
    code[0] = code_address(d, im, cs);
    code[1] = code_address(d, im, vs);
    code[2] = code_address(d, im, fs);
    if (!code[0] || !code[1] || !code[2]) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }

    ogles2_job_begin(ctx);

    /* State buffer: uniform streams, vertex data, the record. */
    update_state_vars(ctx, p);
    memset(&tst, 0, sizeof(tst));
    {
        const struct OGLES2Variant *vv[3];
        vv[0] = cs; vv[1] = vs; vv[2] = fs;
        if (!ubo_copies(ctx, p, vv, &tst))
            goto nomem;
    }
    if (!texture_states(ctx, p, &tst))
        goto nomem;
    unif[0] = write_uniforms(ctx, p, cs, &tst);
    unif[1] = write_uniforms(ctx, p, vs, &tst);
    unif[2] = write_uniforms(ctx, p, fs, &tst);
    defattr = default_attribs(ctx, d);
    if (!unif[0] || !unif[1] || !unif[2] || !defattr)
        goto nomem;
    /* One attribute record per vertex input slot, from the generic attribute
     * bound to it (a matrix: one slot and one generic attribute per column). */
    for (a = 0; a < im->nattribs; a++)
        if (im->attribs[a].slot != 0xff && im->attribs[a].slot + im->attribs[a].columns > nslots)
            nslots = im->attribs[a].slot + im->attribs[a].columns;
    for (i = 0; i < nslots; i++) {
        GLuint g = 0xff;
        BOOL ok;
        for (a = 0; a < im->nattribs; a++) {
            const struct OGLES2ImageAttrib *at = &im->attribs[a];
            if (at->slot != 0xff && i >= at->slot && i < at->slot + at->columns)
                g = p->attrib_index[a] + (i - at->slot);
        }
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
    if (!(rec = shader_record(ctx, defattr, code, unif, cs, vs, fs, mode, src, nslots)))
        goto nomem;
    if (el) {
        /* The indices, rebased to the copied range: 8-bit as they are,
         * 16- and 32-bit swapped to little-endian (plan 7.4). */
        ULONG isize = (ULONG)el->count * index_size(el->type), k;
        UBYTE *ib = claim(v, isize + 4, 4);
        if (!ib)
            goto nomem;
        if (el->type == GL_UNSIGNED_INT) {
            const ULONG *in = el->indices;
            for (k = 0; k < (ULONG)el->count; k++)
                ((ULONG *)ib)[k] = le32(in[k] - el->min);
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
    }

    /* Binning list. */
    v->current_buf = &v->binning_buf[v->build_slot];
    emit_state(ctx, &d->dd, (int)mode, fs);
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
    return;

nomem:
    ogles2_log("glDrawArrays: out of memory for the draw's GPU data; frame dropped");
    v->frame_corrupted = TRUE;
    ogles2_error(ctx, GL_OUT_OF_MEMORY);
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
    struct OGLES2Program *p;

    if (!ogles2_context_valid(ctx))
        return;
    if (mode > GL_TRIANGLE_FAN) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (first < 0 || count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    p = ctx->program;
    if (!p || !ctx->v3d || count == 0) /* no program: undefined; nothing is drawn */
        return;
    if (!arrays_ok(ctx, p, (ULONG)first + (ULONG)count, "glDrawArrays"))
        return;
    draw_arrays(ctx, p, mode, first, count, 0);
}

void ogles2_glDrawElements(struct OGLES2Context *ctx, GLenum mode, GLsizei count, GLenum type,
                           const void *indices)
{
    struct OGLES2Program *p;
    struct elements el;
    ULONG lo = ~0UL, hi = 0, k;

    if (!ogles2_context_valid(ctx))
        return;
    if (mode > GL_TRIANGLE_FAN ||
        (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    p = ctx->program;
    if (!p || !ctx->v3d || count == 0)
        return;
    {
        /* With an element buffer bound, `indices` is the offset into it. */
        BOOL bad;
        const void *in = ogles2_buffer_indices(ctx, (ULONG)indices,
                                               (ULONG)count * index_size(type), &bad);
        if (bad) {
            ogles2_log("glDrawElements: the indices lie outside the element buffer; nothing drawn");
            return;
        }
        if (in)
            indices = in;
        else if (!indices)
            return;
    }
    /* Mesa's state tracker: copy only the range the indices use. */
    for (k = 0; k < (ULONG)count; k++) {
        ULONG x = type == GL_UNSIGNED_INT     ? ((const ULONG *)indices)[k]
                : type == GL_UNSIGNED_SHORT ? ((const UWORD *)indices)[k]
                                            : ((const UBYTE *)indices)[k];
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    if (!arrays_ok(ctx, p, hi + 1, "glDrawElements"))
        return;
    el.indices = indices;
    el.type = type;
    el.count = count;
    el.min = lo;
    draw_arrays(ctx, p, mode, (GLint)lo, (GLsizei)(hi - lo + 1), &el);
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

    if ((mask & GL_DEPTH_BUFFER_BIT) && (!ctx->depth_bits || !s->depth_mask))
        mask &= ~(GLbitfield)GL_DEPTH_BUFFER_BIT;
    if ((mask & GL_STENCIL_BUFFER_BIT) && !ctx->stencil_bits)
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
        memcpy(d->quad.cbuf[0], identity, sizeof(identity));
    }
    quad = &d->quad;
    quad->attrib_index[0] = OGLES2_MAX_VERTEX_ATTRIBS - 2;
    quad->attrib_index[1] = OGLES2_MAX_VERTEX_ATTRIBS - 1;

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

    /* The state: only the scissor test stays the application's. */
    s->caps &= OGLES2_CAP_SCISSOR_TEST;
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
    ctx->viewport[0] = ctx->viewport[1] = 0;
    ctx->viewport[2] = (GLint)ctx->width;
    ctx->viewport[3] = (GLint)ctx->height;

    ogles2_job_begin(ctx);
    draw_arrays(ctx, quad, GL_TRIANGLE_STRIP, 0, 4, 0);

    ctx->state = saved;
    ctx->array[OGLES2_MAX_VERTEX_ATTRIBS - 2] = saved_pos;
    ctx->program = saved_prog;
    for (k = 0; k < 4; k++)
        ctx->viewport[k] = saved_vp[k];
}
