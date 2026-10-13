/*
 * Fixed-function GL state that is pure bookkeeping until M1 draws with it:
 * capabilities, write masks, clear values, line width, depth range, polygon
 * offset, sample coverage, current vertex attributes, and the glGet*
 * queries for them (GLES 2.0 sections 6.1.1-6.1.2).
 *
 * They also carry every awkward argument type of the ABI: GLboolean
 * arguments and returns, and float arguments, which tests/abi_probe checks
 * from both GCC and vbcc programs (plan section 6.3).
 */
#include "ogles2_internal.h"


static ULONG cap_bit(GLenum cap)
{
    switch (cap) {
    case GL_CULL_FACE:                return OGLES2_CAP_CULL_FACE;
    case GL_BLEND:                    return OGLES2_CAP_BLEND;
    case GL_DITHER:                   return OGLES2_CAP_DITHER;
    case GL_STENCIL_TEST:             return OGLES2_CAP_STENCIL_TEST;
    case GL_DEPTH_TEST:               return OGLES2_CAP_DEPTH_TEST;
    case GL_SCISSOR_TEST:             return OGLES2_CAP_SCISSOR_TEST;
    case GL_POLYGON_OFFSET_FILL:      return OGLES2_CAP_POLYGON_OFFSET_FILL;
    case GL_SAMPLE_ALPHA_TO_COVERAGE: return OGLES2_CAP_SAMPLE_ALPHA_TO_COVERAGE;
    case GL_SAMPLE_COVERAGE:          return OGLES2_CAP_SAMPLE_COVERAGE;
    default:                          return 0;
    }
}

static GLfloat clamp01(GLfloat v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

void ogles2_state_init(struct OGLES2Context *ctx)
{
    struct OGLES2State *s = &ctx->state;
    int i;

    s->caps = OGLES2_CAP_DITHER; /* the only capability enabled initially */
    s->depth_mask = GL_TRUE;
    for (i = 0; i < 4; i++) {
        s->color_mask[i] = GL_TRUE;
        s->clear_color[i] = 0.0f;
    }
    s->clear_depth = 1.0f;
    s->line_width = 1.0f;
    s->generate_mipmap_hint = GL_DONT_CARE;
    s->derivative_hint = GL_DONT_CARE;
    s->polygon_mode = GL_FILL;
    s->provoking_vertex = GL_LAST_VERTEX_CONVENTION;
    s->depth_range[0] = 0.0f;
    s->depth_range[1] = 1.0f;
    s->polygon_offset_factor = s->polygon_offset_units = 0.0f;
    s->sample_coverage_value = 1.0f;
    s->sample_coverage_invert = GL_FALSE;
    for (i = 0; i < OGLES2_MAX_VERTEX_ATTRIBS; i++) {
        s->attrib[i][0] = s->attrib[i][1] = s->attrib[i][2] = 0.0f;
        s->attrib[i][3] = 1.0f;
        ctx->array[i].size = 4;
        ctx->array[i].type = GL_FLOAT;
    }
    s->cull_face_mode = GL_BACK;
    s->front_face = GL_CCW;
    s->depth_func = GL_LESS;
    s->blend_src_rgb = s->blend_src_alpha = GL_ONE;
    s->blend_dst_rgb = s->blend_dst_alpha = GL_ZERO;
    s->blend_eq_rgb = s->blend_eq_alpha = GL_FUNC_ADD;
    for (i = 0; i < 4; i++)
        s->blend_color[i] = 0.0f;
    for (i = 0; i < 2; i++) {
        s->stencil_func[i] = GL_ALWAYS;
        s->stencil_fail[i] = s->stencil_zfail[i] = s->stencil_zpass[i] = GL_KEEP;
        s->stencil_ref[i] = 0;
        s->stencil_value_mask[i] = s->stencil_write_mask[i] = ~0U;
    }
    s->clear_stencil = 0;
    ctx->unpack_alignment = ctx->pack_alignment = 4;
}

BOOL ogles2_state_dither(struct OGLES2Context *ctx)
{
    return (ctx->state.caps & OGLES2_CAP_DITHER) ? TRUE : FALSE;
}

/* ---- capabilities ---- */

static void set_cap(struct OGLES2Context *ctx, GLenum cap, int on)
{
    ULONG bit = cap_bit(cap);
    if (!ogles2_context_valid(ctx))
        return;
    if (!bit) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(ctx->state.caps & bit) == !on)
        return; /* unchanged: gl4es disables what is off on every draw */
    if (on)
        ctx->state.caps |= bit;
    else
        ctx->state.caps &= ~bit;
    ctx->state_ver++;
}

void ogles2_glEnable(struct OGLES2Context *ctx, GLenum cap)
{
    set_cap(ctx, cap, 1);
}

void ogles2_glDisable(struct OGLES2Context *ctx, GLenum cap)
{
    set_cap(ctx, cap, 0);
}

GLboolean ogles2_glIsEnabled(struct OGLES2Context *ctx, GLenum cap)
{
    ULONG bit = cap_bit(cap);
    if (!ogles2_context_valid(ctx))
        return GL_FALSE;
    if (!bit) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return GL_FALSE;
    }
    return (ctx->state.caps & bit) ? GL_TRUE : GL_FALSE;
}

/* ---- masks, clear values, rasterisation parameters ---- */

void ogles2_glDepthMask(struct OGLES2Context *ctx, GLboolean flag)
{
    if (ogles2_context_valid(ctx)) {
        ctx->state.depth_mask = flag ? GL_TRUE : GL_FALSE;
        ctx->state_ver++;
    }
}

void ogles2_glColorMask(struct OGLES2Context *ctx, GLboolean red, GLboolean green,
                        GLboolean blue, GLboolean alpha)
{
    if (!ogles2_context_valid(ctx))
        return;
    ctx->state.color_mask[0] = red ? GL_TRUE : GL_FALSE;
    ctx->state.color_mask[1] = green ? GL_TRUE : GL_FALSE;
    ctx->state.color_mask[2] = blue ? GL_TRUE : GL_FALSE;
    ctx->state.color_mask[3] = alpha ? GL_TRUE : GL_FALSE;
    ctx->state_ver++;
}

void ogles2_glClearColor(struct OGLES2Context *ctx, GLfloat red, GLfloat green, GLfloat blue,
                         GLfloat alpha)
{
    if (!ogles2_context_valid(ctx))
        return;
    ctx->state.clear_color[0] = clamp01(red);
    ctx->state.clear_color[1] = clamp01(green);
    ctx->state.clear_color[2] = clamp01(blue);
    ctx->state.clear_color[3] = clamp01(alpha);
}

void ogles2_glClearDepthf(struct OGLES2Context *ctx, GLfloat d)
{
    if (ogles2_context_valid(ctx))
        ctx->state.clear_depth = clamp01(d);
}

void ogles2_glLineWidth(struct OGLES2Context *ctx, GLfloat width)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (!(width > 0.0f)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    ctx->state.line_width = width;
    ctx->state_ver++;
}

void ogles2_glDepthRangef(struct OGLES2Context *ctx, GLfloat n, GLfloat f)
{
    if (!ogles2_context_valid(ctx))
        return;
    ctx->state.depth_range[0] = clamp01(n);
    ctx->state.depth_range[1] = clamp01(f);
    ctx->state_ver++;
}

void ogles2_glPolygonOffset(struct OGLES2Context *ctx, GLfloat factor, GLfloat units)
{
    if (!ogles2_context_valid(ctx))
        return;
    ctx->state.polygon_offset_factor = factor;
    ctx->state.polygon_offset_units = units;
    ctx->state_ver++;
}

void ogles2_glSampleCoverage(struct OGLES2Context *ctx, GLfloat value, GLboolean invert)
{
    if (!ogles2_context_valid(ctx))
        return;
    ctx->state.sample_coverage_value = clamp01(value);
    ctx->state.sample_coverage_invert = invert ? GL_TRUE : GL_FALSE;
}

/* ---- current vertex attributes ---- */

static void set_attrib(struct OGLES2Context *ctx, GLuint index, GLfloat x, GLfloat y, GLfloat z,
                       GLfloat w)
{
    GLfloat *a;
    if (!ogles2_context_valid(ctx))
        return;
    if (index >= OGLES2_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    a = ctx->state.attrib[index];
    a[0] = x;
    a[1] = y;
    a[2] = z;
    a[3] = w;
}

void ogles2_glVertexAttrib1f(struct OGLES2Context *ctx, GLuint index, GLfloat x)
{
    set_attrib(ctx, index, x, 0.0f, 0.0f, 1.0f);
}

void ogles2_glVertexAttrib2f(struct OGLES2Context *ctx, GLuint index, GLfloat x, GLfloat y)
{
    set_attrib(ctx, index, x, y, 0.0f, 1.0f);
}

void ogles2_glVertexAttrib3f(struct OGLES2Context *ctx, GLuint index, GLfloat x, GLfloat y,
                             GLfloat z)
{
    set_attrib(ctx, index, x, y, z, 1.0f);
}

void ogles2_glVertexAttrib4f(struct OGLES2Context *ctx, GLuint index, GLfloat x, GLfloat y,
                             GLfloat z, GLfloat w)
{
    set_attrib(ctx, index, x, y, z, w);
}

void ogles2_glVertexAttrib1fv(struct OGLES2Context *ctx, GLuint index, const GLfloat *v)
{
    if (v)
        set_attrib(ctx, index, v[0], 0.0f, 0.0f, 1.0f);
}

void ogles2_glVertexAttrib2fv(struct OGLES2Context *ctx, GLuint index, const GLfloat *v)
{
    if (v)
        set_attrib(ctx, index, v[0], v[1], 0.0f, 1.0f);
}

void ogles2_glVertexAttrib3fv(struct OGLES2Context *ctx, GLuint index, const GLfloat *v)
{
    if (v)
        set_attrib(ctx, index, v[0], v[1], v[2], 1.0f);
}

void ogles2_glVertexAttrib4fv(struct OGLES2Context *ctx, GLuint index, const GLfloat *v)
{
    if (v)
        set_attrib(ctx, index, v[0], v[1], v[2], v[3]);
}

/* ---- culling, depth function, blending ---- */

void ogles2_glCullFace(struct OGLES2Context *ctx, GLenum mode)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->state.cull_face_mode = mode;
    ctx->state_ver++;
}

void ogles2_glFrontFace(struct OGLES2Context *ctx, GLenum mode)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (mode != GL_CW && mode != GL_CCW) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->state.front_face = mode;
    ctx->state_ver++;
}

/* AmigaOS 4's glPolygonMode: triangles drawn as their edges or corners
 * (CFG_BITS' Direct3D wireframe and point-fill modes, as Mesa's
 * v3dx_emit.c). The hardware has one mode for both faces, so only
 * GL_FRONT_AND_BACK is taken (desktop GL 3.2 core's rule too). */
void ogles2_glPolygonMode(struct OGLES2Context *ctx, GLenum face, GLenum mode)
{
    if (!ogles2_context_valid(ctx))
        return;
    if ((face != GL_FRONT_AND_BACK && face != GL_FRONT && face != GL_BACK) ||
        (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (face != GL_FRONT_AND_BACK) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    ctx->state.polygon_mode = mode;
    ctx->state_ver++;
}

/* AmigaOS 4's glProvokingVertex (EXT_provoking_vertex): the vertex a flat
 * varying takes its value from. GLSL ES 1.00 has no flat varyings, so it is
 * only state and CFG_BITS' Direct3D provoking-vertex bit. */
void ogles2_glProvokingVertex(struct OGLES2Context *ctx, GLenum mode)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (mode != GL_FIRST_VERTEX_CONVENTION && mode != GL_LAST_VERTEX_CONVENTION) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->state.provoking_vertex = mode;
    ctx->state_ver++;
}

void ogles2_glDepthFunc(struct OGLES2Context *ctx, GLenum func)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (func < GL_NEVER || func > GL_ALWAYS) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->state.depth_func = func;
    ctx->state_ver++;
}

static int blend_factor_ok(GLenum f, int is_dst)
{
    switch (f) {
    case GL_ZERO: case GL_ONE:
    case GL_SRC_COLOR: case GL_ONE_MINUS_SRC_COLOR: case GL_DST_COLOR: case GL_ONE_MINUS_DST_COLOR:
    case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA: case GL_DST_ALPHA: case GL_ONE_MINUS_DST_ALPHA:
    case GL_CONSTANT_COLOR: case GL_ONE_MINUS_CONSTANT_COLOR:
    case GL_CONSTANT_ALPHA: case GL_ONE_MINUS_CONSTANT_ALPHA:
        return 1;
    case GL_SRC_ALPHA_SATURATE: /* source only in GLES 2.0 */
        return !is_dst;
    default:
        return 0;
    }
}

void ogles2_glBlendFuncSeparate(struct OGLES2Context *ctx, GLenum srcRGB, GLenum dstRGB,
                                GLenum srcAlpha, GLenum dstAlpha)
{
    struct OGLES2State *s;
    if (!ogles2_context_valid(ctx))
        return;
    if (!blend_factor_ok(srcRGB, 0) || !blend_factor_ok(dstRGB, 1) ||
        !blend_factor_ok(srcAlpha, 0) || !blend_factor_ok(dstAlpha, 1)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    s = &ctx->state;
    s->blend_src_rgb = srcRGB;
    s->blend_dst_rgb = dstRGB;
    s->blend_src_alpha = srcAlpha;
    s->blend_dst_alpha = dstAlpha;
    ctx->state_ver++;
}

void ogles2_glBlendFunc(struct OGLES2Context *ctx, GLenum sfactor, GLenum dfactor)
{
    ogles2_glBlendFuncSeparate(ctx, sfactor, dfactor, sfactor, dfactor);
}

static int blend_eq_ok(GLenum e)
{
    return e == GL_FUNC_ADD || e == GL_FUNC_SUBTRACT || e == GL_FUNC_REVERSE_SUBTRACT ||
           e == GL_MIN_EXT || e == GL_MAX_EXT; /* EXT_blend_minmax */
}

void ogles2_glBlendEquationSeparate(struct OGLES2Context *ctx, GLenum modeRGB, GLenum modeAlpha)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (!blend_eq_ok(modeRGB) || !blend_eq_ok(modeAlpha)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->state.blend_eq_rgb = modeRGB;
    ctx->state.blend_eq_alpha = modeAlpha;
    ctx->state_ver++;
}

void ogles2_glBlendEquation(struct OGLES2Context *ctx, GLenum mode)
{
    ogles2_glBlendEquationSeparate(ctx, mode, mode);
}

void ogles2_glBlendColor(struct OGLES2Context *ctx, GLfloat red, GLfloat green, GLfloat blue,
                         GLfloat alpha)
{
    GLfloat *c;
    if (!ogles2_context_valid(ctx))
        return;
    c = ctx->state.blend_color;
    c[0] = clamp01(red);
    c[1] = clamp01(green);
    c[2] = clamp01(blue);
    c[3] = clamp01(alpha);
    ctx->state_ver++;
}

/* ---- stencil ---- */

/* Faces a GL_FRONT/GL_BACK/GL_FRONT_AND_BACK argument selects, as bits
 * (1 front, 2 back); 0 for anything else. */
static int faces(GLenum face)
{
    return face == GL_FRONT ? 1 : face == GL_BACK ? 2 : face == GL_FRONT_AND_BACK ? 3 : 0;
}

void ogles2_glStencilFuncSeparate(struct OGLES2Context *ctx, GLenum face, GLenum func, GLint ref,
                                  GLuint mask)
{
    int f, i;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(f = faces(face)) || func < GL_NEVER || func > GL_ALWAYS) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    for (i = 0; i < 2; i++)
        if (f & (1 << i)) {
            ctx->state.stencil_func[i] = func;
            ctx->state.stencil_ref[i] = ref;
            ctx->state.stencil_value_mask[i] = mask;
        }
    ctx->state_ver++;
}

void ogles2_glStencilFunc(struct OGLES2Context *ctx, GLenum func, GLint ref, GLuint mask)
{
    ogles2_glStencilFuncSeparate(ctx, GL_FRONT_AND_BACK, func, ref, mask);
}

void ogles2_glStencilMaskSeparate(struct OGLES2Context *ctx, GLenum face, GLuint mask)
{
    int f, i;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(f = faces(face))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    for (i = 0; i < 2; i++)
        if (f & (1 << i))
            ctx->state.stencil_write_mask[i] = mask;
    ctx->state_ver++;
}

void ogles2_glStencilMask(struct OGLES2Context *ctx, GLuint mask)
{
    ogles2_glStencilMaskSeparate(ctx, GL_FRONT_AND_BACK, mask);
}

static int stencil_op_ok(GLenum op)
{
    switch (op) {
    case GL_ZERO: case GL_KEEP: case GL_REPLACE: case GL_INCR: case GL_DECR:
    case GL_INVERT: case GL_INCR_WRAP: case GL_DECR_WRAP:
        return 1;
    default:
        return 0;
    }
}

void ogles2_glStencilOpSeparate(struct OGLES2Context *ctx, GLenum face, GLenum sfail, GLenum dpfail,
                                GLenum dppass)
{
    int f, i;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(f = faces(face)) || !stencil_op_ok(sfail) || !stencil_op_ok(dpfail) ||
        !stencil_op_ok(dppass)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    for (i = 0; i < 2; i++)
        if (f & (1 << i)) {
            ctx->state.stencil_fail[i] = sfail;
            ctx->state.stencil_zfail[i] = dpfail;
            ctx->state.stencil_zpass[i] = dppass;
        }
    ctx->state_ver++;
}

void ogles2_glStencilOp(struct OGLES2Context *ctx, GLenum fail, GLenum zfail, GLenum zpass)
{
    ogles2_glStencilOpSeparate(ctx, GL_FRONT_AND_BACK, fail, zfail, zpass);
}

void ogles2_glClearStencil(struct OGLES2Context *ctx, GLint s)
{
    if (ogles2_context_valid(ctx))
        ctx->state.clear_stencil = s;
}

/* ---- queries ----
 * Each query yields up to four values of one kind; glGetBooleanv,
 * glGetIntegerv and glGetFloatv convert them as GLES 2.0 section 6.1.2 says.
 * "Normalised" floats (colours, depth values) map linearly to the full
 * integer range in glGetIntegerv; others round to the nearest integer. */

enum kind { K_BOOL, K_INT, K_FLOAT, K_NFLOAT };
struct value { enum kind kind; int n; GLint i[4]; GLfloat f[4]; };

static int lookup(struct OGLES2Context *ctx, GLenum pname, struct value *v)
{
    const struct OGLES2State *s = &ctx->state;
    int k;
    ULONG bit = cap_bit(pname);

    if (bit) {
        v->kind = K_BOOL;
        v->n = 1;
        v->i[0] = (s->caps & bit) != 0;
        return 1;
    }
    switch (pname) {
    case GL_DEPTH_WRITEMASK:
        v->kind = K_BOOL; v->n = 1; v->i[0] = s->depth_mask;
        return 1;
    case GL_COLOR_WRITEMASK:
        v->kind = K_BOOL; v->n = 4;
        for (k = 0; k < 4; k++)
            v->i[k] = s->color_mask[k];
        return 1;
    case GL_SAMPLE_COVERAGE_INVERT:
        v->kind = K_BOOL; v->n = 1; v->i[0] = s->sample_coverage_invert;
        return 1;
    case GL_COLOR_CLEAR_VALUE:
        v->kind = K_NFLOAT; v->n = 4;
        for (k = 0; k < 4; k++)
            v->f[k] = s->clear_color[k];
        return 1;
    case GL_DEPTH_CLEAR_VALUE:
        v->kind = K_NFLOAT; v->n = 1; v->f[0] = s->clear_depth;
        return 1;
    case GL_DEPTH_RANGE:
        v->kind = K_NFLOAT; v->n = 2; v->f[0] = s->depth_range[0]; v->f[1] = s->depth_range[1];
        return 1;
    case GL_LINE_WIDTH:
        v->kind = K_FLOAT; v->n = 1; v->f[0] = s->line_width;
        return 1;
    case GL_POLYGON_OFFSET_FACTOR:
        v->kind = K_FLOAT; v->n = 1; v->f[0] = s->polygon_offset_factor;
        return 1;
    case GL_POLYGON_OFFSET_UNITS:
        v->kind = K_FLOAT; v->n = 1; v->f[0] = s->polygon_offset_units;
        return 1;
    case GL_SAMPLE_COVERAGE_VALUE:
        v->kind = K_FLOAT; v->n = 1; v->f[0] = s->sample_coverage_value;
        return 1;
    case GL_MAX_VERTEX_ATTRIBS:
        v->kind = K_INT; v->n = 1; v->i[0] = OGLES2_MAX_VERTEX_ATTRIBS;
        return 1;
    /* The compiler's (Mesa's v3d_screen.c): a 64 KB constant buffer per
     * stage, 4096 vec4; V3D_MAX_FS_INPUTS / 4 varyings. */
    case GL_MAX_VERTEX_UNIFORM_VECTORS:
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS:
        v->kind = K_INT; v->n = 1; v->i[0] = 4096;
        return 1;
    case GL_MAX_VARYING_VECTORS:
        v->kind = K_INT; v->n = 1; v->i[0] = 16;
        return 1;
    case GL_VIEWPORT:
        v->kind = K_INT; v->n = 4;
        for (k = 0; k < 4; k++)
            v->i[k] = ctx->viewport[k];
        return 1;
    case GL_SCISSOR_BOX:
        v->kind = K_INT; v->n = 4;
        for (k = 0; k < 4; k++)
            v->i[k] = ctx->scissor[k];
        return 1;
    case GL_MAX_VIEWPORT_DIMS:
        v->kind = K_INT; v->n = 2; v->i[0] = v->i[1] = OGLES2_MAX_VIEWPORT;
        return 1;
    case GL_GENERATE_MIPMAP_HINT:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->generate_mipmap_hint;
        return 1;
    case GL_FRAGMENT_SHADER_DERIVATIVE_HINT_OES:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->derivative_hint;
        return 1;
    case GL_POLYGON_MODE: /* front and back: one mode for both (glPolygonMode) */
        v->kind = K_INT; v->n = 2; v->i[0] = v->i[1] = (GLint)s->polygon_mode;
        return 1;
    case GL_PROVOKING_VERTEX:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->provoking_vertex;
        return 1;
    case GL_QUADS_FOLLOW_PROVOKING_VERTEX_CONVENTION: /* no quads in GLES2 */
        v->kind = K_BOOL; v->n = 1; v->i[0] = 0;
        return 1;
    case GL_NUM_PROGRAM_BINARY_FORMATS_OES:
        v->kind = K_INT; v->n = 1; v->i[0] = 1;
        return 1;
    case GL_PROGRAM_BINARY_FORMATS_OES:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)GL_PROGRAM_BINARY_V3DP_PISTORM;
        return 1;
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT:
        v->kind = K_INT; v->n = 1; v->i[0] = GL_RGBA;
        return 1;
    case GL_IMPLEMENTATION_COLOR_READ_TYPE:
        v->kind = K_INT; v->n = 1; v->i[0] = GL_UNSIGNED_BYTE;
        return 1;
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS: /* OES_compressed_ETC1_RGB8_texture */
        v->kind = K_INT; v->n = 1; v->i[0] = 1;
        return 1;
    case GL_COMPRESSED_TEXTURE_FORMATS:
        v->kind = K_INT; v->n = 1; v->i[0] = 0x8D64; /* GL_ETC1_RGB8_OES */
        return 1;
    case GL_SHADER_COMPILER: /* TRUE with offline binaries too (plan 7.8) */
        v->kind = K_BOOL; v->n = 1; v->i[0] = 1;
        return 1;
    case GL_NUM_SHADER_BINARY_FORMATS:
        v->kind = K_INT; v->n = 1; v->i[0] = 0;
        return 1;
    case GL_RED_BITS:
    case GL_GREEN_BITS:
    case GL_BLUE_BITS:
    case GL_ALPHA_BITS: /* the default framebuffer is RGBA8 (plan 7.2) */
    case GL_DEPTH_BITS:
    case GL_STENCIL_BITS:
        v->kind = K_INT; v->n = 1;
        if (ctx->draw_fb) /* a framebuffer object's (fbo.c) */
            ogles2_fbo_query(ctx, pname, &v->i[0]);
        else
            v->i[0] = pname == GL_DEPTH_BITS ? ctx->depth_bits
                    : pname == GL_STENCIL_BITS ? ctx->stencil_bits : ctx->v3d ? 8 : 0;
        return 1;
    case GL_CULL_FACE_MODE:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->cull_face_mode;
        return 1;
    case GL_FRONT_FACE:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->front_face;
        return 1;
    case GL_DEPTH_FUNC:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->depth_func;
        return 1;
    case GL_BLEND_SRC_RGB:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->blend_src_rgb;
        return 1;
    case GL_BLEND_DST_RGB:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->blend_dst_rgb;
        return 1;
    case GL_BLEND_SRC_ALPHA:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->blend_src_alpha;
        return 1;
    case GL_BLEND_DST_ALPHA:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->blend_dst_alpha;
        return 1;
    case GL_BLEND_EQUATION_RGB: /* = GL_BLEND_EQUATION */
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->blend_eq_rgb;
        return 1;
    case GL_BLEND_EQUATION_ALPHA:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->blend_eq_alpha;
        return 1;
    case GL_BLEND_COLOR:
        v->kind = K_NFLOAT; v->n = 4;
        for (k = 0; k < 4; k++)
            v->f[k] = s->blend_color[k];
        return 1;
    case GL_STENCIL_FUNC:
    case GL_STENCIL_BACK_FUNC:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->stencil_func[pname == GL_STENCIL_BACK_FUNC];
        return 1;
    case GL_STENCIL_REF:
    case GL_STENCIL_BACK_REF:
        v->kind = K_INT; v->n = 1; v->i[0] = s->stencil_ref[pname == GL_STENCIL_BACK_REF];
        return 1;
    case GL_STENCIL_VALUE_MASK:
    case GL_STENCIL_BACK_VALUE_MASK:
        v->kind = K_INT; v->n = 1;
        v->i[0] = (GLint)s->stencil_value_mask[pname == GL_STENCIL_BACK_VALUE_MASK];
        return 1;
    case GL_STENCIL_WRITEMASK:
    case GL_STENCIL_BACK_WRITEMASK:
        v->kind = K_INT; v->n = 1;
        v->i[0] = (GLint)s->stencil_write_mask[pname == GL_STENCIL_BACK_WRITEMASK];
        return 1;
    case GL_STENCIL_FAIL:
    case GL_STENCIL_BACK_FAIL:
        v->kind = K_INT; v->n = 1; v->i[0] = (GLint)s->stencil_fail[pname == GL_STENCIL_BACK_FAIL];
        return 1;
    case GL_STENCIL_PASS_DEPTH_FAIL:
    case GL_STENCIL_BACK_PASS_DEPTH_FAIL:
        v->kind = K_INT; v->n = 1;
        v->i[0] = (GLint)s->stencil_zfail[pname == GL_STENCIL_BACK_PASS_DEPTH_FAIL];
        return 1;
    case GL_STENCIL_PASS_DEPTH_PASS:
    case GL_STENCIL_BACK_PASS_DEPTH_PASS:
        v->kind = K_INT; v->n = 1;
        v->i[0] = (GLint)s->stencil_zpass[pname == GL_STENCIL_BACK_PASS_DEPTH_PASS];
        return 1;
    case GL_STENCIL_CLEAR_VALUE:
        v->kind = K_INT; v->n = 1; v->i[0] = s->clear_stencil;
        return 1;
    case GL_CURRENT_PROGRAM:
        v->kind = K_INT; v->n = 1; v->i[0] = ctx->program ? (GLint)ctx->program->name : 0;
        return 1;
    default:
        if (ogles2_texture_query(ctx, pname, &v->i[0]) || ogles2_buffer_query(ctx, pname, &v->i[0]) ||
            ogles2_fbo_query(ctx, pname, &v->i[0])) {
            v->kind = K_INT;
            v->n = 1;
            return 1;
        }
        return 0;
    }
}

static int query(struct OGLES2Context *ctx, GLenum pname, struct value *v, const char *fn)
{
    if (!ogles2_context_valid(ctx))
        return 0;
    if (!lookup(ctx, pname, v)) {
        /* Not every query is answered yet (limits, bindings, ...): say which. */
        ogles2_log("%s: pname 0x%04lx not supported yet", (ULONG)fn, (ULONG)pname);
        ogles2_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    return 1;
}

static GLint float_to_int(GLfloat f, int normalised)
{
    double d = normalised ? (double)f * 2147483647.0 : (double)f;
    d += d < 0 ? -0.5 : 0.5;
    if (d >= 2147483647.0)
        return 2147483647;
    if (d <= -2147483648.0)
        return (GLint)-2147483647 - 1;
    return (GLint)d;
}

void ogles2_glGetBooleanv(struct OGLES2Context *ctx, GLenum pname, GLboolean *data)
{
    struct value v;
    int k;
    if (!data || !query(ctx, pname, &v, "glGetBooleanv"))
        return;
    for (k = 0; k < v.n; k++)
        data[k] = (v.kind == K_FLOAT || v.kind == K_NFLOAT) ? (v.f[k] != 0.0f) : (v.i[k] != 0);
}

void ogles2_glGetIntegerv(struct OGLES2Context *ctx, GLenum pname, GLint *data)
{
    struct value v;
    int k;
    if (!data || !query(ctx, pname, &v, "glGetIntegerv"))
        return;
    for (k = 0; k < v.n; k++)
        data[k] = (v.kind == K_FLOAT || v.kind == K_NFLOAT) ? float_to_int(v.f[k], v.kind == K_NFLOAT)
                                                             : v.i[k];
}

void ogles2_glGetFloatv(struct OGLES2Context *ctx, GLenum pname, GLfloat *data)
{
    struct value v;
    int k;
    if (!data || !query(ctx, pname, &v, "glGetFloatv"))
        return;
    for (k = 0; k < v.n; k++)
        data[k] = (v.kind == K_FLOAT || v.kind == K_NFLOAT) ? v.f[k] : (GLfloat)v.i[k];
}

void ogles2_glGetVertexAttribfv(struct OGLES2Context *ctx, GLuint index, GLenum pname,
                                GLfloat *params)
{
    int k;
    if (!ogles2_context_valid(ctx) || !params)
        return;
    if (index >= OGLES2_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (pname != GL_CURRENT_VERTEX_ATTRIB) {
        /* The array state (enabled, size, stride...) arrives with M1. */
        ogles2_unimplemented(ctx, "glGetVertexAttribfv (array state)");
        return;
    }
    for (k = 0; k < 4; k++)
        params[k] = ctx->state.attrib[index][k];
}
