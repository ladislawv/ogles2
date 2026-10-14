/*
 * Shader and program objects (GLES 2.0 section 2.10), attribute and uniform
 * locations, and uniform storage (plan section 7.8).
 *
 * glCompileShader keeps the source and succeeds (S1, plan 7.8). glLinkProgram
 * hashes the sources (v3dp/FORMAT.md, PROGRAM: the vertex shader, then the
 * fragment shader) and loads <hash>.v3dp from PROGDIR: or OGLES2Cache:, the
 * name being the hash's first 24 hex digits (FFS allows 30 characters); the
 * full hash in the file must match. A
 * program with no binary fails to link; its sources go to
 * T:ogles2_missing_shaders.txt for the offline compiler; with the on-target
 * compiler installed (LIBS:v3dglslc, compiler.c) the program is compiled
 * into the disk cache first. Loaded images stay
 * in the context's program cache, by hash, until the context goes: a
 * relink, or another program with the same sources, reuses them (plan 7.8).
 *
 * Uniform storage is one constant buffer per stage, as in Mesa: glUniform*
 * writes each value into every stage that uses it (the .v3dp's storage
 * places), and the uniform recipes read from there at draw time.
 *
 * One image is built in: PiStorm3D's smooth-shaded shaders (builtin_shaders.h)
 * with a hand-written description, used for clears drawn as rectangles.
 */
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "ogles2_internal.h"
#include "builtin_shaders.h"
#include "blake3.h"

/* ---- the built-in image ---- */

static const struct OGLES2ImageAttrib smooth_attribs[] = {
    {"a_position", GL_FLOAT_VEC3, 0, 1, 0}, /* VPM inputs 0-2, both shaders */
    {"a_color", GL_FLOAT_VEC4, 1, 1, 0},    /* VPM inputs 3-6, vertex shader only */
};

static const struct OGLES2Place smooth_mvp_place[] = {{0, 16, 4, 0, 0}};

static const struct OGLES2ImageUniform smooth_uniforms[] = {
    {"u_mvp", GL_FLOAT_MAT4, 0, 0, 1, smooth_mvp_place, {~0UL, ~0UL}},
};

/* The vertex and coordinate shaders read the same stream (v3d_assembler.c:
 * scale_p, scale_p_y, the matrix, then the depth range's scale and offset,
 * which the coordinate shader does not reach). The matrix goes row by row:
 * the shader forms x' = x M00 + y M10 + z M20 + w M30 from the first four
 * words, so they are row 0 of the column-major GL matrix (words 0, 4, 8, 12). */
static const ULONG smooth_rtype[20] = {
    OGLES2_U_PISTORM_VP_WIDTH, OGLES2_U_PISTORM_VP_HEIGHT,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    OGLES2_U_VIEWPORT_Z_SCALE, OGLES2_U_VIEWPORT_Z_OFFSET,
};
static const ULONG smooth_rdata[20] = {0, 0, 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15, 0, 0};

/* The record values PiStorm3D's d_BuildShaderRecord uses for this pairing,
 * expressed as prog_data: 4 threads, the vertex and coordinate shaders start
 * in their final thread section, VPM output segments of 1 (coordinate) and 2
 * (vertex), a VCM cache of 4 batches, four varyings using the pixel centre w. */
static const struct OGLES2Variant smooth_variants[3] = {
    {OGLES2_VARIANT_CS, 4, 1, 0, 0, 4, 0, 0, 0, {3}, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     {0}, {0}, {0}, 0, builtin_coordinate, sizeof(builtin_coordinate), 0, 20, smooth_rtype,
     smooth_rdata, 0, 0, 0, 0, 0, 0},
    {OGLES2_VARIANT_VS, 4, 1, 0, 0, 4, 0, 0, 0, {3, 4}, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     {0}, {0}, {0}, 0, builtin_vertex_smooth, sizeof(builtin_vertex_smooth),
     sizeof(builtin_coordinate), 20, smooth_rtype, smooth_rdata, 0, 0, 0, 0, 0, 0},
    {OGLES2_VARIANT_FS, 4, 0, 0, 0, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0,
     {0}, {0}, {0}, 0, builtin_fragment_smooth, sizeof(builtin_fragment_smooth),
     sizeof(builtin_coordinate) + sizeof(builtin_vertex_smooth), 0, 0, 0, 0, 0, 0, 0, 0,
     1 /* render target 0 */},
};

static const struct OGLES2Image builtin_smooth = {
    "built-in smooth colour (PiStorm3D VERTEX_SMOOTH, FRAGMENT_UNTEXTURED_SMOOTH)",
    {0},
    3, smooth_variants,
    2, smooth_attribs,
    1, smooth_uniforms,
    {16, 0}, {0, 0},
    0, 0,
    sizeof(builtin_coordinate) + sizeof(builtin_vertex_smooth) + sizeof(builtin_fragment_smooth),
    0,          /* no OGLES2_U_CLIP_Z_COEFF: clears are drawn NEGATIVE_ONE_TO_ONE */
    0, 0, 0,
    0, 0,       /* no binary */
    0, 0, 0, 0, /* no 32-bit-sampler variants, no sources */
};

const struct OGLES2Image *ogles2_builtin_image(void)
{
    return &builtin_smooth;
}

/* ---- small string helpers (the library has no C library) ---- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* <0, 0, >0 as strcmp (bytes unsigned). */
static int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (int)(UBYTE)*a - (int)(UBYTE)*b;
}

static ULONG str_len(const char *s)
{
    ULONG n = 0;
    while (s[n])
        n++;
    return n;
}

/* Copies src into dst[size] like glGet*InfoLog: truncated, NUL-terminated. */
static GLsizei copy_out(char *dst, GLsizei size, const char *src)
{
    GLsizei n = 0;
    if (!dst || size <= 0)
        return 0;
    while (src && src[n] && n < size - 1) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    return n;
}

/* ---- the name table ---- */

void *ogles2_object(struct OGLES2Context *ctx, GLuint name, UBYTE kind)
{
    UBYTE *o;
    if (name == 0 || name >= ctx->share->nobjects)
        return 0;
    o = ctx->share->objects[name];
    return (o && *o == kind) ? o : 0;
}

static BOOL name_exists(struct OGLES2Context *ctx, GLuint name)
{
    return name && name < ctx->share->nobjects && ctx->share->objects[name];
}

static GLuint new_name(struct OGLES2Context *ctx, void *obj)
{
    GLuint i;
    for (i = 1; i < ctx->share->nobjects; i++)
        if (!ctx->share->objects[i]) {
            ctx->share->objects[i] = obj;
            return i;
        }
    {
        GLuint n = ctx->share->nobjects ? ctx->share->nobjects * 2 : 16;
        void **t = AllocVec(n * sizeof(void *), MEMF_ANY | MEMF_CLEAR);
        if (!t)
            return 0;
        for (i = 0; i < ctx->share->nobjects; i++)
            t[i] = ctx->share->objects[i];
        if (ctx->share->objects)
            FreeVec(ctx->share->objects);
        i = ctx->share->nobjects ? ctx->share->nobjects : 1;
        ctx->share->objects = t;
        ctx->share->nobjects = n;
        ctx->share->objects[i] = obj;
        return i;
    }
}

static void free_shader(struct OGLES2Context *ctx, struct OGLES2Shader *s)
{
    ctx->share->objects[s->name] = 0;
    if (s->source)
        FreeVec(s->source);
    if (s->info_log)
        FreeVec(s->info_log);
    FreeVec(s);
}

/* A shader marked for deletion goes once no program holds it. */
static void release_shader(struct OGLES2Context *ctx, struct OGLES2Shader *s)
{
    if (!s)
        return;
    s->attached--;
    if (s->delete_pending && !s->attached)
        free_shader(ctx, s);
}

static void unlink_program(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    int k;
    for (k = 0; k < 2; k++) {
        if (p->cbuf[k])
            FreeVec(p->cbuf[k]);
        if (p->cbuf_le[k])
            FreeVec(p->cbuf_le[k]);
        p->cbuf[k] = p->cbuf_le[k] = 0;
    }
    if (p->loc_uniform)
        FreeVec(p->loc_uniform);
    if (p->sampler_unit)
        FreeVec(p->sampler_unit);
    if (p->uniform_loc)
        FreeVec(p->uniform_loc);
    p->uniform_loc = 0;
    p->loc_uniform = p->loc_element = 0;
    p->sampler_unit = 0;
    p->nlocations = 0;
    if (p->rec_tmpl)
        FreeVec(p->rec_tmpl);
    p->rec_tmpl = 0;
    p->var_ok = FALSE;
    p->nsamplers = 0xff;
    p->image = 0; /* images belong to the context's cache */
    p->linked = GL_FALSE;
    (void)ctx;
}

void ogles2_cbuf_set(struct OGLES2Program *p, int stage, ULONG at, ULONG w)
{
    p->cbuf_ver[stage]++; /* draw.c's kept uniform streams are made again */
    p->cbuf[stage][at] = w;
    p->cbuf_le[stage][at] = (w >> 24) | ((w >> 8) & 0xff00UL) | ((w << 8) & 0xff0000UL) | (w << 24);
}

static void set_log(struct OGLES2Program *p, const char *a, const char *b)
{
    ULONG la = str_len(a), lb = b ? str_len(b) : 0;
    if (p->info_log)
        FreeVec(p->info_log);
    p->info_log = AllocVec(la + lb + 1, MEMF_ANY);
    if (p->info_log) {
        memcpy(p->info_log, a, la);
        if (lb)
            memcpy(p->info_log + la, b, lb);
        p->info_log[la + lb] = 0;
    }
}

static void free_program(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    ctx->share->objects[p->name] = 0;
    release_shader(ctx, p->vs);
    release_shader(ctx, p->fs);
    unlink_program(ctx, p);
    if (p->info_log)
        FreeVec(p->info_log);
    FreeVec(p);
}

/* Is p the current program of a member of ctx's share group? */
static BOOL in_use(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    struct OGLES2Context *m;
    for (m = ctx->share->first; m; m = m->share_next)
        if (m->program == p)
            return TRUE;
    return FALSE;
}

/* aglDestroyContext (its jobs finished): its current program let go of; the
 * shaders, programs and program cache with the share group's last member. */
void ogles2_objects_free(struct OGLES2Context *ctx)
{
    struct OGLES2Program *old = ctx->program;
    GLuint i;
    UBYTE *o;
    ctx->program = 0;
    if (ctx->share->members > 1) {
        if (old && old->delete_pending && !in_use(ctx, old))
            free_program(ctx, old);
        return;
    }
    for (i = 1; i < ctx->share->nobjects; i++) /* programs first: they hold shaders */
        if ((o = ctx->share->objects[i]) && *o == OGLES2_OBJ_PROGRAM)
            free_program(ctx, (struct OGLES2Program *)o);
    for (i = 1; i < ctx->share->nobjects; i++)
        if ((o = ctx->share->objects[i]))
            free_shader(ctx, (struct OGLES2Shader *)o);
    if (ctx->share->objects)
        FreeVec(ctx->share->objects);
    ctx->share->objects = 0;
    ctx->share->nobjects = 0;
    while (ctx->share->images) {
        struct OGLES2Image *im = ctx->share->images;
        ctx->share->images = im->next;
        ogles2_image_free(im, ctx);
    }
}

/* Looks a name up for a call that wants `kind`: GL_INVALID_VALUE for no such
 * object, GL_INVALID_OPERATION for an object of the other kind. */
static void *lookup(struct OGLES2Context *ctx, GLuint name, UBYTE kind)
{
    void *o = ogles2_object(ctx, name, kind);
    if (!o)
        ogles2_error(ctx, name_exists(ctx, name) ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
    return o;
}

/* ---- shaders ---- */

GLuint ogles2_glCreateShader(struct OGLES2Context *ctx, GLenum type)
{
    struct OGLES2Shader *s;
    if (!ogles2_context_valid(ctx))
        return 0;
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    s = AllocVec(sizeof(*s), MEMF_ANY | MEMF_CLEAR);
    if (!s || !(s->name = new_name(ctx, s))) {
        if (s)
            FreeVec(s);
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return 0;
    }
    s->kind = OGLES2_OBJ_SHADER;
    s->type = type;
    return s->name;
}

void ogles2_glDeleteShader(struct OGLES2Context *ctx, GLuint shader)
{
    struct OGLES2Shader *s;
    if (!ogles2_context_valid(ctx) || shader == 0)
        return;
    if (!(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    s->delete_pending = TRUE;
    if (!s->attached)
        free_shader(ctx, s);
}

GLboolean ogles2_glIsShader(struct OGLES2Context *ctx, GLuint shader)
{
    if (!ogles2_context_valid(ctx))
        return GL_FALSE;
    return ogles2_object(ctx, shader, OGLES2_OBJ_SHADER) ? GL_TRUE : GL_FALSE;
}

void ogles2_glShaderSource(struct OGLES2Context *ctx, GLuint shader, GLsizei count,
                           const GLchar *const *string, const GLint *length)
{
    struct OGLES2Shader *s;
    ULONG total = 0, n;
    GLsizei i;
    char *src;
    if (!ogles2_context_valid(ctx))
        return;
    if (count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    for (i = 0; i < count; i++)
        if (string[i])
            total += (length && length[i] >= 0) ? (ULONG)length[i] : str_len(string[i]);
    if (!(src = AllocVec(total + 1, MEMF_ANY))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    total = 0;
    for (i = 0; i < count; i++) {
        if (!string[i])
            continue;
        n = (length && length[i] >= 0) ? (ULONG)length[i] : str_len(string[i]);
        memcpy(src + total, string[i], n);
        total += n;
    }
    src[total] = 0;
    if (s->source)
        FreeVec(s->source);
    s->source = src;
}

static void shader_record_path(char *path, const struct OGLES2Shader *s);
static int read_record(const char *path, ULONG compiler, char **log);
static void write_record(const char *path, const char *log);
static void write_fail_record(const char *path, ULONG compiler, const char *log);
static char *dup_text(const char *t, ULONG n);

/* With the on-target compiler (plan 8.4) a shader is compiled here, for
 * its GL_COMPILE_STATUS and info log; its record in the disk cache
 * (shader_record) answers for it next time without the compiler. Without
 * the compiler (S1, plan 7.8) the source is only kept and linking does the
 * real work. */
void ogles2_glCompileShader(struct OGLES2Context *ctx, GLuint shader)
{
    struct OGLES2Shader *s;
    char path[96], *log;
    ULONG compiler;
    int rc;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    if (s->info_log)
        FreeVec(s->info_log);
    s->info_log = 0;
    s->compiled = GL_FALSE;
    if (!s->source)
        return;
    if (!(compiler = ogles2_compiler_id())) {
        s->compiled = GL_TRUE;
        return;
    }
    shader_record_path(path, s);
    /* A record: compiled before, or failed with this compiler (gl4es tries
     * shaders for optional features at every start that fail by design: the
     * compiler need not be started for them, about a second). */
    rc = read_record(path, compiler, &s->info_log);
    if (rc >= 0) {
        s->compiled = rc ? GL_TRUE : GL_FALSE;
        return;
    }
    if (!(log = AllocVec(4096, MEMF_ANY | MEMF_CLEAR))) {
        s->compiled = GL_TRUE; /* checked again at link */
        return;
    }
    rc = ogles2_compile_shader(s->type == GL_FRAGMENT_SHADER, s->source, log, 4096);
    if (rc < 0) {
        s->compiled = GL_TRUE; /* no compiler after all: as S1, the link decides */
    } else {
        s->compiled = rc ? GL_TRUE : GL_FALSE;
        if (log[0])
            s->info_log = dup_text(log, str_len(log));
        if (rc)
            write_record(path, log);
        else
            write_fail_record(path, compiler, log);
    }
    FreeVec(log);
}

void ogles2_glGetShaderiv(struct OGLES2Context *ctx, GLuint shader, GLenum pname, GLint *params)
{
    struct OGLES2Shader *s;
    if (!ogles2_context_valid(ctx) || !params)
        return;
    if (!(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    switch (pname) {
    case GL_SHADER_TYPE:          *params = (GLint)s->type; break;
    case GL_DELETE_STATUS:        *params = s->delete_pending ? GL_TRUE : GL_FALSE; break;
    case GL_COMPILE_STATUS:       *params = s->compiled; break;
    case GL_INFO_LOG_LENGTH:      *params = s->info_log ? (GLint)str_len(s->info_log) + 1 : 0; break;
    case GL_SHADER_SOURCE_LENGTH: *params = s->source ? (GLint)str_len(s->source) + 1 : 0; break;
    default:                      ogles2_error(ctx, GL_INVALID_ENUM); break;
    }
}

void ogles2_glGetShaderInfoLog(struct OGLES2Context *ctx, GLuint shader, GLsizei bufSize,
                               GLsizei *length, GLchar *infoLog)
{
    struct OGLES2Shader *s;
    GLsizei n;
    if (!ogles2_context_valid(ctx))
        return;
    if (bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    n = copy_out(infoLog, bufSize, s->info_log ? s->info_log : "");
    if (length)
        *length = n;
}

void ogles2_glGetShaderSource(struct OGLES2Context *ctx, GLuint shader, GLsizei bufSize,
                              GLsizei *length, GLchar *source)
{
    struct OGLES2Shader *s;
    GLsizei n;
    if (!ogles2_context_valid(ctx))
        return;
    if (bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    n = copy_out(source, bufSize, s->source ? s->source : "");
    if (length)
        *length = n;
}

/* ---- programs ---- */

GLuint ogles2_glCreateProgram(struct OGLES2Context *ctx)
{
    struct OGLES2Program *p;
    if (!ogles2_context_valid(ctx))
        return 0;
    p = AllocVec(sizeof(*p), MEMF_ANY | MEMF_CLEAR);
    if (!p || !(p->name = new_name(ctx, p))) {
        if (p)
            FreeVec(p);
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return 0;
    }
    p->kind = OGLES2_OBJ_PROGRAM;
    return p->name;
}

void ogles2_glDeleteProgram(struct OGLES2Context *ctx, GLuint program)
{
    struct OGLES2Program *p;
    if (!ogles2_context_valid(ctx) || program == 0)
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    p->delete_pending = TRUE;
    if (!in_use(ctx, p)) /* a current program goes when it stops being current, in every member */
        free_program(ctx, p);
}

GLboolean ogles2_glIsProgram(struct OGLES2Context *ctx, GLuint program)
{
    if (!ogles2_context_valid(ctx))
        return GL_FALSE;
    return ogles2_object(ctx, program, OGLES2_OBJ_PROGRAM) ? GL_TRUE : GL_FALSE;
}

void ogles2_glAttachShader(struct OGLES2Context *ctx, GLuint program, GLuint shader)
{
    struct OGLES2Program *p;
    struct OGLES2Shader *s, **slot;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)) ||
        !(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    slot = s->type == GL_VERTEX_SHADER ? &p->vs : &p->fs;
    if (*slot) { /* already attached, or one of that type already is */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    *slot = s;
    s->attached++;
}

void ogles2_glDetachShader(struct OGLES2Context *ctx, GLuint program, GLuint shader)
{
    struct OGLES2Program *p;
    struct OGLES2Shader *s;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)) ||
        !(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    if (p->vs == s)
        p->vs = 0;
    else if (p->fs == s)
        p->fs = 0;
    else {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    release_shader(ctx, s);
}

void ogles2_glGetAttachedShaders(struct OGLES2Context *ctx, GLuint program, GLsizei maxCount,
                                 GLsizei *count, GLuint *shaders)
{
    struct OGLES2Program *p;
    GLsizei n = 0;
    if (!ogles2_context_valid(ctx))
        return;
    if (maxCount < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (p->vs && n < maxCount)
        shaders[n++] = p->vs->name;
    if (p->fs && n < maxCount)
        shaders[n++] = p->fs->name;
    if (count)
        *count = n;
}

void ogles2_glBindAttribLocation(struct OGLES2Context *ctx, GLuint program, GLuint index,
                                 const GLchar *name)
{
    struct OGLES2Program *p;
    int i;
    if (!ogles2_context_valid(ctx) || !name)
        return;
    if (index >= OGLES2_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (name[0] == 'g' && name[1] == 'l' && name[2] == '_') {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    for (i = 0; i < p->nbindings; i++)
        if (str_eq(p->bind_name[i], name))
            break;
    if (i == p->nbindings) {
        if (p->nbindings == OGLES2_MAX_BINDINGS || str_len(name) >= sizeof(p->bind_name[0])) {
            ogles2_log("glBindAttribLocation: \"%s\" not kept (too many or too long)", (ULONG)name);
            return;
        }
        p->nbindings++;
    }
    copy_out(p->bind_name[i], sizeof(p->bind_name[i]), name);
    p->bind_index[i] = index;
}

/* ---- linking ---- */

/* GLES 2.0 2.10.4: two attribute names bound to one location (aliasing),
 * which links only when Mesa's linker applies the bindings (else it gives
 * every active attribute a location of its own, and more than 16 fail). The
 * program's bindings then, "<index> <name>\n" lines sorted by name
 * (AllocVec'd); 0 for a program without aliasing (its bindings are applied
 * at runtime, by name: the binary does not depend on them). */
static char *aliased_bindings(const struct OGLES2Program *p)
{
    UBYTE order[OGLES2_MAX_BINDINGS];
    int i, j, n = p->nbindings;
    BOOL alias = FALSE;
    ULONG size = 1, at = 0;
    char *t;
    for (i = 0; i < n && !alias; i++)
        for (j = i + 1; j < n; j++)
            if (p->bind_index[i] == p->bind_index[j]) {
                alias = TRUE;
                break;
            }
    if (!alias)
        return 0;
    for (i = 0; i < n; i++) { /* insertion sort by name */
        for (j = i; j > 0 && str_cmp(p->bind_name[order[j - 1]], p->bind_name[i]) > 0; j--)
            order[j] = order[j - 1];
        order[j] = (UBYTE)i;
        size += 12 + str_len(p->bind_name[i]);
    }
    if (!(t = AllocVec(size, MEMF_ANY)))
        return 0;
    for (i = 0; i < n; i++)
        at += ogles2_format(t + at, size - at, "%lu %s\n", (ULONG)p->bind_index[order[i]],
                            (ULONG)p->bind_name[order[i]]);
    t[at] = 0;
    return t;
}

/* The program hash (v3dp/FORMAT.md, PROGRAM), as hex: per shader, vertex
 * first, its stage and length as little-endian 32-bit words, then its text;
 * then aliased bindings the same way, as the stage V3DP_HASH_BINDINGS. */
static void program_hash(const struct OGLES2Program *p, const char *bindings, UBYTE out[32], char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    struct blake3 h;
    int k;
    blake3_init(&h);
    for (k = 0; k < (bindings ? 3 : 2); k++) {
        const char *src = k == 2 ? bindings : k ? p->fs->source : p->vs->source;
        ULONG stage = k == 2 ? 0x42494E44UL /* V3DP_HASH_BINDINGS */ : k ? 4 : 0, len = str_len(src), i;
        UBYTE w[8];
        for (i = 0; i < 4; i++) {
            w[i] = (UBYTE)(stage >> (8 * i));
            w[4 + i] = (UBYTE)(len >> (8 * i));
        }
        blake3_update(&h, w, 8);
        blake3_update(&h, src, len);
    }
    blake3_final(&h, out);
    for (k = 0; k < 32; k++) {
        hex[2 * k] = digits[out[k] >> 4];
        hex[2 * k + 1] = digits[out[k] & 15];
    }
    hex[64] = 0;
}

/* Reads a whole file; NULL if it does not exist. */
static UBYTE *read_file(const char *path, ULONG *len)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    UBYTE *buf = 0;
    LONG size;
    if (!fh)
        return 0;
    if (Seek(fh, 0, OFFSET_END) >= 0 && (size = Seek(fh, 0, OFFSET_BEGINNING)) > 0 &&
        size < 16L * 1024 * 1024 && (buf = AllocVec((ULONG)size, MEMF_ANY))) {
        if (Read(fh, buf, size) != size) {
            FreeVec(buf);
            buf = 0;
        }
        *len = (ULONG)size;
    }
    Close(fh);
    return buf;
}

/* ---- shader records (plan 8.4): <24 hex digits>.v3ds in the disk cache,
 * one per shader the compiler accepted, holding its info log, or refused,
 * holding its errors. The hash is BLAKE3 over the shader's stage and
 * source, as for a one-shader program; the first line tags the file (a
 * record with no messages is not empty): "v3ds 1", or "v3ds 0 <the
 * compiler's id, 8 hex digits>" (ogles2_compiler_id: another compiler
 * tries the shader again). */
#define RECORD_TAG "v3ds 1\n"
#define FAIL_TAG "v3ds 0 "

static void shader_record_path(char *path, const struct OGLES2Shader *s)
{
    static const char digits[] = "0123456789abcdef";
    struct blake3 h;
    UBYTE out[32], w[8];
    ULONG stage = s->type == GL_FRAGMENT_SHADER ? 4 : 0, len = str_len(s->source), i;
    char *q;
    blake3_init(&h);
    for (i = 0; i < 4; i++) {
        w[i] = (UBYTE)(stage >> (8 * i));
        w[4 + i] = (UBYTE)(len >> (8 * i));
    }
    blake3_update(&h, w, 8);
    blake3_update(&h, s->source, len);
    blake3_final(&h, out);
    strcpy(path, ogles2_cache_dir());
    q = path + str_len(path);
    for (i = 0; i < 12; i++) { /* 24 digits + ".v3ds": FFS's 30 characters */
        *q++ = digits[out[i] >> 4];
        *q++ = digits[out[i] & 15];
    }
    strcpy(q, ".v3ds");
}

static char *dup_text(const char *t, ULONG n)
{
    char *d = AllocVec(n + 1, MEMF_ANY);
    if (d) {
        memcpy(d, t, n);
        d[n] = 0;
    }
    return d;
}

static void hex8(char *d, ULONG x)
{
    static const char digits[] = "0123456789abcdef";
    int k;
    for (k = 7; k >= 0; k--, x >>= 4)
        d[k] = digits[x & 15];
    d[8] = 0;
}

/* 1: compiled (its log in *log), 0: refused by `compiler` (its errors), -1:
 * no record, or one of another compiler's refusals. */
static int read_record(const char *path, ULONG compiler, char **log)
{
    ULONG len = 0, tag = str_len(RECORD_TAG), ftag = str_len(FAIL_TAG) + 9;
    UBYTE *file = read_file(path, &len);
    char id[12];
    int rc = -1;
    *log = 0;
    if (!file)
        return -1;
    hex8(id, compiler);
    id[8] = '\n';
    if (len >= tag && !memcmp(file, RECORD_TAG, tag)) {
        rc = 1;
        *log = len > tag ? dup_text((const char *)file + tag, len - tag) : 0;
    } else if (len >= ftag && !memcmp(file, FAIL_TAG, ftag - 9) && !memcmp(file + ftag - 9, id, 9)) {
        rc = 0;
        *log = len > ftag ? dup_text((const char *)file + ftag, len - ftag) : 0;
    }
    FreeVec(file);
    return rc;
}

static void write_fail_record(const char *path, ULONG compiler, const char *log)
{
    BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
    char id[12];
    if (!fh)
        return;
    hex8(id, compiler);
    FPuts(fh, (STRPTR)FAIL_TAG);
    FPuts(fh, (STRPTR)id);
    FPuts(fh, (STRPTR) "\n");
    if (log && log[0])
        FPuts(fh, (STRPTR)log);
    Close(fh);
}

static void write_record(const char *path, const char *log)
{
    BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh)
        return;
    FPuts(fh, (STRPTR)RECORD_TAG);
    if (log && log[0])
        FPuts(fh, (STRPTR)log);
    Close(fh);
}

static void mark_compiled(const struct OGLES2Shader *s)
{
    char path[96], *log;
    int rc;
    shader_record_path(path, s);
    rc = read_record(path, ogles2_compiler_id(), &log);
    if (log)
        FreeVec(log);
    if (rc == 1)
        return;
    write_record(path, s->info_log); /* also over a refusal: it linked after all */
}

/* Appends a program without a binary to T:ogles2_missing_shaders.txt, for
 * the offline compiler (plan 7.8). */
static void note_missing(const struct OGLES2Program *p, const char *hex)
{
    BPTR fh = Open((STRPTR) "T:ogles2_missing_shaders.txt", MODE_READWRITE);
    if (!fh)
        return;
    Seek(fh, 0, OFFSET_END);
    FPuts(fh, (STRPTR) "--- program ");
    FPuts(fh, (STRPTR)hex);
    FPuts(fh, (STRPTR) "\n--- vertex shader\n");
    FPuts(fh, (STRPTR)p->vs->source);
    FPuts(fh, (STRPTR) "\n--- fragment shader\n");
    FPuts(fh, (STRPTR)p->fs->source);
    FPuts(fh, (STRPTR) "\n--- end\n");
    Close(fh);
}

#define FILE_HASH_DIGITS 24 /* + ".v3dp": 29 characters, FFS's limit is 30 */

/* Reads and checks one .v3dp file: the image, or 0 (with *refused set and
 * the program's log when the file exists but is not usable). */
static struct OGLES2Image *try_file(struct OGLES2Program *p, const UBYTE *hash, const char *path,
                                    BOOL *refused)
{
    char why[120];
    struct OGLES2Image *im;
    UBYTE *file;
    ULONG len = 0;
    *refused = FALSE;
    if (!(file = read_file(path, &len)))
        return 0;
    im = ogles2_v3dp_load(file, len, why, sizeof(why));
    if (im) { /* kept: glGetProgramBinaryOES hands it out */
        im->binary = file;
        im->binary_len = len;
    } else {
        FreeVec(file);
    }
    if (im && memcmp(im->hash, hash, 32)) {
        /* The same first 24 digits, another program: not ours. */
        ogles2_log("glLinkProgram: %s is another program (hash differs)", (ULONG)path);
        ogles2_image_free(im, 0);
        return 0;
    }
    if (im) {
        ogles2_log("glLinkProgram: %s (%ld variants, %ld bytes of code)", (ULONG)path,
                   (LONG)im->nvariants, im->code_total);
        return im;
    }
    ogles2_log("glLinkProgram: %s refused: %s", (ULONG)path, (ULONG)why);
    set_log(p, "the program binary was refused: ", why);
    *refused = TRUE;
    return 0;
}

static void file_path(char *path, const char *dir, const char *hex)
{
    strcpy(path, dir);
    strncat(path, hex, FILE_HASH_DIGITS);
    strcat(path, ".v3dp");
}

/* Plan 7.8: PROGDIR:, the disk cache (OGLES2Cache:, or T: when it is not
 * assigned: that is where compiled programs go then), the on-target
 * compiler, else the sources go to T:ogles2_missing_shaders.txt. */
static struct OGLES2Image *load_image(struct OGLES2Program *p, const UBYTE *hash, const char *hex,
                                      const char *bindings)
{
    char path[96];
    struct OGLES2Image *im;
    BOOL refused;
    const char *dirs[2];
    int d;
    dirs[0] = "PROGDIR:";
    dirs[1] = ogles2_cache_dir();
    for (d = 0; d < 2; d++) {
        file_path(path, dirs[d], hex);
        if ((im = try_file(p, hash, path, &refused)) || refused)
            return im;
    }
    if (ogles2_compiler_installed()) {
        char *log = AllocVec(4096, MEMF_ANY | MEMF_CLEAR);
        BOOL ok;
        if (!log) {
            set_log(p, "out of memory", 0);
            return 0;
        }
        file_path(path, dirs[1], hex);
        ok = bindings ? ogles2_compile_program_bound(p->vs->source, p->fs->source, bindings, path, log, 4096)
                      : ogles2_compile_program(p->vs->source, p->fs->source, path, log, 4096);
        if (ok && (im = try_file(p, hash, path, &refused))) {
            FreeVec(log);
            return im;
        }
        if (!ok) {
            ogles2_log("glLinkProgram: the compiler failed: %s", (ULONG)log);
            set_log(p, log[0] ? log : "the shader compiler could not be run", 0);
        }
        FreeVec(log);
        return 0;
    }
    ogles2_log("glLinkProgram: no binary for program %s (sources in T:ogles2_missing_shaders.txt)",
               (ULONG)hex);
    set_log(p, "no program binary for this program: ", hex);
    note_missing(p, hex);
    return 0;
}

/* A .v3dw file of the program `hash` (its 32-bit-sampler variants), or 0. */
static struct OGLES2Image *try_wide(const UBYTE *hash, const char *path)
{
    char why[120];
    struct OGLES2Image *w;
    UBYTE *file;
    ULONG len = 0;
    if (!(file = read_file(path, &len)))
        return 0;
    w = ogles2_v3dp_load(file, len, why, sizeof(why));
    FreeVec(file);
    if (w && memcmp(w->hash, hash, 32)) {
        ogles2_image_free(w, 0);
        return 0;
    }
    if (!w)
        ogles2_log("draw: %s refused: %s", (ULONG)path, (ULONG)why);
    return w;
}

/* The program's variants for samplers that return 32 bits (a float or
 * depth texture bound, draw.c): its .v3dw beside the .v3dp (PROGDIR:, the
 * disk cache), or compiled now by the on-target compiler into the cache.
 * Once per image: at the first draw that needs them, which waits for the
 * compile (about a second on the A1200). NULL if there are none: such draws
 * then use the 16-bit variants, which read those textures wrongly (logged). */
const struct OGLES2Image *ogles2_image_wide(struct OGLES2Image *im)
{
    static const char digits[] = "0123456789abcdef";
    char hex[FILE_HASH_DIGITS + 1], path[96];
    const char *dirs[2];
    int d, k;
    if (im->wide || im->wide_tried)
        return im->wide;
    im->wide_tried = TRUE;
    for (k = 0; k < FILE_HASH_DIGITS / 2; k++) {
        hex[2 * k] = digits[im->hash[k] >> 4];
        hex[2 * k + 1] = digits[im->hash[k] & 15];
    }
    hex[FILE_HASH_DIGITS] = 0;
    dirs[0] = "PROGDIR:";
    dirs[1] = ogles2_cache_dir();
    for (d = 0; d < 2 && !im->wide; d++) {
        strcpy(path, dirs[d]);
        strcat(path, hex);
        strcat(path, ".v3dw");
        im->wide = try_wide(im->hash, path);
    }
    if (!im->wide && im->vs_src && im->fs_src && ogles2_compiler_installed()) {
        char *log = AllocVec(1024, MEMF_ANY | MEMF_CLEAR);
        if (log && ogles2_compile_program_wide(im->vs_src, im->fs_src, path, log, 1024))
            im->wide = try_wide(im->hash, path);
        else
            ogles2_log("draw: the compiler made no 32-bit sampler variants for program %s: %s", (ULONG)hex,
                       (ULONG)(log ? log : "out of memory"));
        if (log)
            FreeVec(log);
    }
    if (im->wide)
        ogles2_log("draw: program %s: its 32-bit sampler variants (%ld)", (ULONG)hex, (LONG)im->wide->nvariants);
    else
        ogles2_log("draw: program %s samples a float or depth texture but has no 32-bit sampler variants "
                   "(no %s.v3dw): its colours from that texture are wrong", (ULONG)hex, (ULONG)hex);
    return im->wide;
}

/* After an image is set: attribute locations, uniform locations, constant
 * buffers. FALSE (with the log set) if the attributes do not fit. */
static BOOL bind_image(struct OGLES2Program *p, const struct OGLES2Image *im)
{
    ULONG used = 0;
    int a, i, k, n;
    UWORD u, nloc = 0;

    p->var_ok = FALSE;     /* draw.c's per-program caches start empty */
    p->us_var[0] = p->us_var[1] = 0;
    p->nsamplers = 0xff;
    p->nslots = 0xff;

    /* Attribute locations: glBindAttribLocation first, then the lowest free
     * run (a matrix takes one location per column). */
    for (a = 0; a < im->nattribs; a++) {
        p->attrib_index[a] = 0xff;
        for (i = 0; i < p->nbindings; i++)
            if (str_eq(p->bind_name[i], im->attribs[a].name)) {
                p->attrib_index[a] = (UBYTE)p->bind_index[i];
                for (k = 0; k < im->attribs[a].columns; k++)
                    used |= 1UL << (p->bind_index[i] + k);
            }
    }
    for (a = 0; a < im->nattribs; a++) {
        if (p->attrib_index[a] != 0xff)
            continue;
        n = im->attribs[a].columns;
        for (i = 0; i + n <= OGLES2_MAX_VERTEX_ATTRIBS; i++) {
            ULONG run = ((1UL << n) - 1) << i;
            if (!(used & run))
                break;
        }
        if (i + n > OGLES2_MAX_VERTEX_ATTRIBS) {
            set_log(p, "too many vertex attributes", 0);
            return FALSE;
        }
        p->attrib_index[a] = (UBYTE)i;
        used |= ((1UL << n) - 1) << i;
    }

    /* Uniform locations: one per array element of every visible uniform. */
    for (u = 0; u < im->nuniforms; u++)
        if (!(im->uniforms[u].flags & (OGLES2_UNIFORM_BUILTIN | OGLES2_UNIFORM_HIDDEN)))
            nloc += im->uniforms[u].array ? im->uniforms[u].array : 1;
    p->loc_uniform = AllocVec(nloc * 4 + 4, MEMF_ANY);
    /* A sampler array's elements are samplers of their own (consecutive
     * texture indices, as Mesa lowers them): a unit per location. */
    p->sampler_unit = AllocVec(nloc + 1, MEMF_ANY | MEMF_CLEAR);
    p->uniform_loc = AllocVec((im->nuniforms + 1) * sizeof(UWORD), MEMF_ANY);
    for (k = 0; k < 2; k++) {
        p->cbuf[k] = AllocVec(im->cbuf_words[k] * 4 + 4, MEMF_ANY | MEMF_CLEAR);
        p->cbuf_le[k] = AllocVec(im->cbuf_words[k] * 4 + 4, MEMF_ANY | MEMF_CLEAR);
    }
    if (!p->loc_uniform || !p->sampler_unit || !p->uniform_loc || !p->cbuf[0] || !p->cbuf[1] || !p->cbuf_le[0] ||
        !p->cbuf_le[1]) {
        set_log(p, "out of memory", 0);
        return FALSE;
    }
    p->loc_element = p->loc_uniform + nloc;
    nloc = 0;
    for (u = 0; u < im->nuniforms; u++) {
        const struct OGLES2ImageUniform *un = &im->uniforms[u];
        UWORD e, ne = un->array ? un->array : 1;
        p->uniform_loc[u] = 0xFFFF;
        if (un->flags & (OGLES2_UNIFORM_BUILTIN | OGLES2_UNIFORM_HIDDEN))
            continue;
        p->uniform_loc[u] = (UWORD)nloc;
        for (e = 0; e < ne; e++) {
            p->loc_uniform[nloc] = u;
            p->loc_element[nloc] = e;
            nloc++;
        }
    }
    p->nlocations = nloc;
    for (k = 0; k < 2; k++)
        if (im->cbuf_template[k]) {
            UWORD w;
            for (w = 0; w < im->cbuf_words[k]; w++)
                ogles2_cbuf_set(p, k, w, im->cbuf_template[k][w]);
        }
    p->image = im;
    return TRUE;
}

static void describe_variants(struct OGLES2Program *p, const struct OGLES2Image *im, const char *hex);

void ogles2_glLinkProgram(struct OGLES2Context *ctx, GLuint program)
{
    struct OGLES2Program *p;
    struct OGLES2Image *im;
    UBYTE hash[32];
    char hex[65], *bindings;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    /* A program in use keeps its old image in the open job until relinked:
     * the job's records already point at its code (ogles2_image_free defers). */
    unlink_program(ctx, p);
    if (!p->vs || !p->fs || !p->vs->compiled || !p->fs->compiled) {
        /* Say which shader failed, with its compiler messages. */
        if (p->vs && p->fs && !p->vs->compiled)
            set_log(p, "the vertex shader did not compile:\n", p->vs->info_log);
        else if (p->vs && p->fs && !p->fs->compiled)
            set_log(p, "the fragment shader did not compile:\n", p->fs->info_log);
        else
            set_log(p, "a compiled vertex shader and fragment shader must be attached", 0);
        return;
    }
    bindings = aliased_bindings(p);
    program_hash(p, bindings, hash, hex);
    for (im = ctx->share->images; im && memcmp(im->hash, hash, 32); im = im->next)
        ;
    if (!im) {
        im = load_image(p, hash, hex, bindings);
        if (bindings)
            FreeVec(bindings);
        bindings = 0;
        if (!im)
            return;
        im->next = ctx->share->images;
        ctx->share->images = im;
        /* Kept for its 32-bit-sampler variants, compiled when a draw first
         * needs them (ogles2_image_wide). */
        im->vs_src = dup_text(p->vs->source, str_len(p->vs->source));
        im->fs_src = dup_text(p->fs->source, str_len(p->fs->source));
        /* A binary proves both shaders compile: record them, so the next
         * glCompileShader of either needs no compiler. */
        if (ogles2_compiler_installed()) {
            mark_compiled(p->vs);
            mark_compiled(p->fs);
        }
    }
    if (bindings)
        FreeVec(bindings);
    if (!bind_image(p, im)) {
        unlink_program(ctx, p);
        return;
    }
    p->linked = GL_TRUE;
    if (ctx->debug_shader_log)
        describe_variants(p, im, hex);
    else
        set_log(p, "", 0);
}

/* OGLES2_CCT_DEBUG_SHADER_LOG (plan 8.4): the link log describes what the
 * program runs, one line per compiled variant, as Mesa's shader-db
 * statistics do; also in the library's log. */
static void describe_variants(struct OGLES2Program *p, const struct OGLES2Image *im, const char *hex)
{
    ULONG size = 80 + 96UL * im->nvariants, n;
    char *t = AllocVec(size, MEMF_ANY);
    UWORD i;
    if (!t) {
        set_log(p, "", 0);
        return;
    }
    n = ogles2_format(t, size, "program %.24s: %ld variants\n", (ULONG)hex, (LONG)im->nvariants);
    for (i = 0; i < im->nvariants; i++) {
        const struct OGLES2Variant *v = &im->variants[i];
        const char *what = v->stage == OGLES2_VARIANT_FS
                               ? (v->is_points ? "fragment, points" : v->is_lines ? "fragment, lines"
                                                                                  : "fragment, triangles")
                               : v->stage == OGLES2_VARIANT_CS
                                     ? (v->per_vertex_point_size ? "binning, points" : "binning")
                                     : (v->per_vertex_point_size ? "vertex, points" : "vertex");
        n += ogles2_format(t + n, size - n, "  %s: %ld instructions, %ld threads, %ld spill bytes, %ld uniforms\n",
                           (ULONG)what, (LONG)(v->code_bytes / 8), (LONG)v->threads, (LONG)v->spill_size,
                           (LONG)v->nrecipe);
    }
    set_log(p, t, 0);
    ogles2_log("glLinkProgram: %s", (ULONG)t);
    FreeVec(t);
}

/* A program on an image the library carries (draw.c's clears): the
 * built-in one, or one loaded from the library's own data. */
BOOL ogles2_program_image(struct OGLES2Program *p, const struct OGLES2Image *im)
{
    memset(p, 0, sizeof(*p));
    p->kind = OGLES2_OBJ_PROGRAM;
    if (!bind_image(p, im)) {
        ogles2_program_builtin_free(p);
        return FALSE;
    }
    p->linked = GL_TRUE;
    return TRUE;
}

BOOL ogles2_program_builtin(struct OGLES2Program *p)
{
    return ogles2_program_image(p, &builtin_smooth);
}

void ogles2_program_builtin_free(struct OGLES2Program *p)
{
    unlink_program(0, p);
    if (p->info_log)
        FreeVec(p->info_log);
    p->info_log = 0;
}

void ogles2_glUseProgram(struct OGLES2Context *ctx, GLuint program)
{
    struct OGLES2Program *p = 0, *old;
    if (!ogles2_context_valid(ctx))
        return;
    if (program) {
        if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
            return;
        if (!p->linked) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    }
    old = ctx->program;
    ctx->program = p;
    if (old && old != p && old->delete_pending && !in_use(ctx, old))
        free_program(ctx, old);
}

void ogles2_glValidateProgram(struct OGLES2Context *ctx, GLuint program)
{
    if (!ogles2_context_valid(ctx))
        return;
    lookup(ctx, program, OGLES2_OBJ_PROGRAM); /* the status is LINK_STATUS's (glGetProgramiv) */
}

/* The name glGetActiveUniform reports: arrays get "[0]" (GLES 2.0 2.10.4). */
static ULONG active_name_len(const struct OGLES2ImageUniform *u)
{
    return str_len(u->name) + (u->array ? 3 : 0);
}

static BOOL visible(const struct OGLES2ImageUniform *u)
{
    return !(u->flags & (OGLES2_UNIFORM_BUILTIN | OGLES2_UNIFORM_HIDDEN));
}

/* The index'th visible uniform, or NULL. */
static const struct OGLES2ImageUniform *active_uniform(const struct OGLES2Image *im, GLuint index)
{
    UWORD u;
    for (u = 0; u < im->nuniforms; u++)
        if (visible(&im->uniforms[u]) && index-- == 0)
            return &im->uniforms[u];
    return 0;
}

static const struct OGLES2ImageAttrib *active_attrib(const struct OGLES2Image *im, GLuint index)
{
    UWORD a;
    for (a = 0; a < im->nattribs; a++)
        if (im->attribs[a].slot != 0xff && index-- == 0)
            return &im->attribs[a];
    return 0;
}

void ogles2_glGetProgramiv(struct OGLES2Context *ctx, GLuint program, GLenum pname, GLint *params)
{
    struct OGLES2Program *p;
    const struct OGLES2Image *im;
    GLint n, i;
    if (!ogles2_context_valid(ctx) || !params)
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    im = p->image;
    switch (pname) {
    case GL_DELETE_STATUS:     *params = p->delete_pending ? GL_TRUE : GL_FALSE; break;
    case GL_LINK_STATUS:
    case GL_VALIDATE_STATUS:   *params = p->linked; break;
    case GL_INFO_LOG_LENGTH:
        *params = (p->info_log && p->info_log[0]) ? (GLint)str_len(p->info_log) + 1 : 0;
        break;
    case GL_ATTACHED_SHADERS:  *params = (p->vs ? 1 : 0) + (p->fs ? 1 : 0); break;
    case GL_ACTIVE_ATTRIBUTES:
        for (n = 0; im && active_attrib(im, (GLuint)n); n++)
            ;
        *params = n;
        break;
    case GL_ACTIVE_UNIFORMS:
        for (n = 0; im && active_uniform(im, (GLuint)n); n++)
            ;
        *params = n;
        break;
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
        n = 0;
        for (i = 0; im && active_attrib(im, (GLuint)i); i++)
            if ((GLint)str_len(active_attrib(im, (GLuint)i)->name) + 1 > n)
                n = (GLint)str_len(active_attrib(im, (GLuint)i)->name) + 1;
        *params = n;
        break;
    case GL_ACTIVE_UNIFORM_MAX_LENGTH:
        n = 0;
        for (i = 0; im && active_uniform(im, (GLuint)i); i++)
            if ((GLint)active_name_len(active_uniform(im, (GLuint)i)) + 1 > n)
                n = (GLint)active_name_len(active_uniform(im, (GLuint)i)) + 1;
        *params = n;
        break;
    case GL_PROGRAM_BINARY_LENGTH_OES:
        *params = p->linked && im ? (GLint)im->binary_len : 0;
        break;
    default:
        ogles2_error(ctx, GL_INVALID_ENUM);
        break;
    }
}

/* ---- OES_get_program_binary: the binary is the program's .v3dp file
 * (GL_PROGRAM_BINARY_V3DP_PISTORM), as the disk cache holds it. ---- */

void ogles2_glGetProgramBinaryOES(struct OGLES2Context *ctx, GLuint program, GLsizei bufSize,
                                  GLsizei *length, GLenum *binaryFormat, void *binary)
{
    struct OGLES2Program *p;
    const struct OGLES2Image *im;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    im = p->image;
    /* Not linked, or a buffer too small for it (the extension's errors). */
    if (!p->linked || !im || !im->binary || bufSize < (GLsizei)im->binary_len) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        if (length)
            *length = 0;
        return;
    }
    if (binary)
        memcpy(binary, im->binary, im->binary_len);
    if (length)
        *length = (GLsizei)im->binary_len;
    if (binaryFormat)
        *binaryFormat = GL_PROGRAM_BINARY_V3DP_PISTORM;
}

/* The program runs the binary's image: as a link from shaders does, with
 * the binary in place of the disk cache; a binary that is not usable (its
 * reason in the info log) leaves the program not linked, which the
 * extension asks of a binary an implementation refuses. */
void ogles2_glProgramBinaryOES(struct OGLES2Context *ctx, GLuint program, GLenum binaryFormat,
                               const void *binary, GLint length)
{
    struct OGLES2Program *p;
    struct OGLES2Image *im, *c;
    char why[120];
    UBYTE *copy;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (binaryFormat != GL_PROGRAM_BINARY_V3DP_PISTORM) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    unlink_program(ctx, p);
    if (!binary || length <= 0) {
        set_log(p, "the program binary was refused: ", "empty");
        return;
    }
    if (!(im = ogles2_v3dp_load((const UBYTE *)binary, (ULONG)length, why, sizeof(why)))) {
        ogles2_log("glProgramBinaryOES: refused: %s", (ULONG)why);
        set_log(p, "the program binary was refused: ", why);
        return;
    }
    /* The program cache: an image of the same program is already loaded. */
    for (c = ctx->share->images; c && memcmp(c->hash, im->hash, 32); c = c->next)
        ;
    if (c) {
        ogles2_image_free(im, 0);
        im = c;
    } else if ((copy = AllocVec((ULONG)length, MEMF_ANY))) {
        memcpy(copy, binary, (ULONG)length);
        im->binary = copy;
        im->binary_len = (ULONG)length;
        im->next = ctx->share->images;
        ctx->share->images = im;
    } else {
        ogles2_image_free(im, 0);
        set_log(p, "out of memory", 0);
        return;
    }
    if (!bind_image(p, im)) {
        unlink_program(ctx, p);
        return;
    }
    p->linked = GL_TRUE;
    set_log(p, "", 0);
}

void ogles2_glGetProgramInfoLog(struct OGLES2Context *ctx, GLuint program, GLsizei bufSize,
                                GLsizei *length, GLchar *infoLog)
{
    struct OGLES2Program *p;
    GLsizei n;
    if (!ogles2_context_valid(ctx))
        return;
    if (bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    n = copy_out(infoLog, bufSize, p->info_log ? p->info_log : "");
    if (length)
        *length = n;
}

/* Linked program for a location query: GL_INVALID_OPERATION if not linked. */
static struct OGLES2Program *linked(struct OGLES2Context *ctx, GLuint program)
{
    struct OGLES2Program *p = lookup(ctx, program, OGLES2_OBJ_PROGRAM);
    if (p && !p->linked) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return 0;
    }
    return p;
}

GLint ogles2_glGetAttribLocation(struct OGLES2Context *ctx, GLuint program, const GLchar *name)
{
    struct OGLES2Program *p;
    int a;
    if (!ogles2_context_valid(ctx) || !name || !(p = linked(ctx, program)))
        return -1;
    for (a = 0; a < p->image->nattribs; a++)
        if (p->image->attribs[a].slot != 0xff && str_eq(p->image->attribs[a].name, name))
            return p->attrib_index[a];
    return -1;
}

void ogles2_glGetActiveAttrib(struct OGLES2Context *ctx, GLuint program, GLuint index,
                              GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type,
                              GLchar *name)
{
    struct OGLES2Program *p;
    const struct OGLES2ImageAttrib *a;
    GLsizei n;
    if (!ogles2_context_valid(ctx) || !(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (!p->image || !(a = active_attrib(p->image, index)) || bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    n = copy_out(name, bufSize, a->name);
    if (length)
        *length = n;
    if (size)
        *size = 1;
    if (type)
        *type = a->type;
}

/* ---- uniforms ----
 * Locations number every array element of every visible uniform, in the
 * image's order (the .v3dp leaves the numbering to the runtime). */

GLint ogles2_glGetUniformLocation(struct OGLES2Context *ctx, GLuint program, const GLchar *name)
{
    struct OGLES2Program *p;
    const struct OGLES2Image *im;
    ULONG len, base;
    LONG index = -1;
    UWORD loc;
    if (!ogles2_context_valid(ctx) || !name || !(p = linked(ctx, program)))
        return -1;
    im = p->image;
    len = str_len(name);
    base = len;
    /* "a[3]": an array element; "a" and "a[0]" are its first. */
    if (len > 3 && name[len - 1] == ']') {
        ULONG i = len - 2, v = 0, mul = 1;
        while (i > 0 && name[i] >= '0' && name[i] <= '9') {
            v += (ULONG)(name[i] - '0') * mul;
            mul *= 10;
            i--;
        }
        if (name[i] == '[' && i < len - 2) {
            base = i;
            index = (LONG)v;
        }
    }
    for (loc = 0; loc < p->nlocations; loc++) {
        const struct OGLES2ImageUniform *u = &im->uniforms[p->loc_uniform[loc]];
        if (p->loc_element[loc])
            continue;
        if (str_eq(u->name, name))
            return loc; /* the whole name, struct members included */
        if (index >= 0 && u->array && (LONG)u->array > index && str_len(u->name) == base &&
            !strncmp(u->name, name, base))
            return loc + (GLint)index;
    }
    return -1;
}

void ogles2_glGetActiveUniform(struct OGLES2Context *ctx, GLuint program, GLuint index,
                               GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type,
                               GLchar *name)
{
    struct OGLES2Program *p;
    const struct OGLES2ImageUniform *u;
    GLsizei n;
    if (!ogles2_context_valid(ctx) || !(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (!p->image || !(u = active_uniform(p->image, index)) || bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    n = copy_out(name, bufSize, u->name);
    if (u->array && name && n + 3 < bufSize) {
        name[n++] = '[';
        name[n++] = '0';
        name[n++] = ']';
        name[n] = 0;
    }
    if (length)
        *length = n;
    if (size)
        *size = u->array ? u->array : 1;
    if (type)
        *type = u->type;
}

/* Kinds of uniform data. */
#define K_FLOAT 0
#define K_INT   1
#define K_BOOL  2
#define K_SAMPLER 3

/* Kind, components per column and columns of a GL uniform type; 0 if unknown. */
static int type_shape(GLenum t, int *kind, int *comps, int *cols)
{
    *cols = 1;
    switch (t) {
    case GL_FLOAT:        *kind = K_FLOAT; *comps = 1; return 1;
    case GL_FLOAT_VEC2:   *kind = K_FLOAT; *comps = 2; return 1;
    case GL_FLOAT_VEC3:   *kind = K_FLOAT; *comps = 3; return 1;
    case GL_FLOAT_VEC4:   *kind = K_FLOAT; *comps = 4; return 1;
    case GL_INT:          *kind = K_INT;   *comps = 1; return 1;
    case GL_INT_VEC2:     *kind = K_INT;   *comps = 2; return 1;
    case GL_INT_VEC3:     *kind = K_INT;   *comps = 3; return 1;
    case GL_INT_VEC4:     *kind = K_INT;   *comps = 4; return 1;
    case GL_BOOL:         *kind = K_BOOL;  *comps = 1; return 1;
    case GL_BOOL_VEC2:    *kind = K_BOOL;  *comps = 2; return 1;
    case GL_BOOL_VEC3:    *kind = K_BOOL;  *comps = 3; return 1;
    case GL_BOOL_VEC4:    *kind = K_BOOL;  *comps = 4; return 1;
    case GL_FLOAT_MAT2:   *kind = K_FLOAT; *comps = 2; *cols = 2; return 1;
    case GL_FLOAT_MAT3:   *kind = K_FLOAT; *comps = 3; *cols = 3; return 1;
    case GL_FLOAT_MAT4:   *kind = K_FLOAT; *comps = 4; *cols = 4; return 1;
    case GL_SAMPLER_2D:
    case GL_SAMPLER_CUBE:
    case GL_SAMPLER_2D_RECT: /* ARB_texture_rectangle */
        *kind = K_SAMPLER; *comps = 1; return 1;
    default:              return 0;
    }
}

static ULONG fbits(GLfloat f)
{
    union { GLfloat f; ULONG u; } c;
    c.f = f;
    return c.u;
}

/* glUniform* and glUniformMatrix*: `count` values of `comps` x `cols`
 * components, floats (fv) or ints (iv), into the current program's uniform
 * at `location`. `kind` is the call's (K_FLOAT for f, K_INT for i). */
static void set_uniform(struct OGLES2Context *ctx, GLint location, GLsizei count, int kind,
                        int comps, int cols, const GLfloat *fv, const GLint *iv)
{
    struct OGLES2Program *p;
    const struct OGLES2ImageUniform *u;
    int ukind, ucomps, ucols, c, k, pl;
    UWORD e, first, ne;
    GLsizei i;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(p = ctx->program)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (location == -1) /* silently ignored (GLES 2.0 section 2.10.4) */
        return;
    if (count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (location < 0 || location >= p->nlocations) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    u = &p->image->uniforms[p->loc_uniform[location]];
    first = p->loc_element[location];
    if (!type_shape(u->type, &ukind, &ucomps, &ucols) || ucomps != comps || ucols != cols ||
        (count > 1 && !u->array) ||
        !(ukind == kind || ukind == K_BOOL || (ukind == K_SAMPLER && kind == K_INT && comps == 1))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!count || (!fv && !iv))
        return;
    ne = u->array ? u->array : 1;
    if (count > ne - first)
        count = ne - first; /* past the array's end: ignored (GLES 2.0 2.10.4) */
    if (ukind == K_SAMPLER) {
        for (i = 0; i < count; i++)
            if (iv[i] < 0 || iv[i] >= OGLES2_MAX_TEXTURE_UNITS) { /* a unit, 0-31 (combined) */
                ogles2_error(ctx, GL_INVALID_VALUE);
                return;
            }
        for (i = 0; i < count; i++)
            p->sampler_unit[location + i] = (UBYTE)iv[i];
        return;
    }
    for (pl = 0; pl < u->nplaces; pl++) {
        const struct OGLES2Place *place = &u->places[pl];
        for (i = 0, e = first; i < count; i++, e++)
            for (c = 0; c < cols; c++)
                for (k = 0; k < comps; k++) {
                    int src = (i * cols + c) * comps + k;
                    ULONG w;
                    ULONG at = place->offset + (ULONG)e * place->elem_stride + (ULONG)c * place->vec_stride + k;
                    if (ukind == K_BOOL) {
                        BOOL b = kind == K_FLOAT ? fv[src] != 0.0f : iv[src] != 0;
                        w = place->int_as_float ? (b ? fbits(1.0f) : 0) : (b ? ~0UL : 0);
                    } else if (ukind == K_INT) {
                        w = place->int_as_float ? fbits((GLfloat)iv[src]) : (ULONG)iv[src];
                    } else {
                        w = fbits(fv[src]);
                    }
                    if (at < p->image->cbuf_words[place->stage])
                        ogles2_cbuf_set(p, place->stage, at, w);
                }
    }
}

void ogles2_glUniform1fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLfloat *value)
{
    set_uniform(ctx, location, count, K_FLOAT, 1, 1, value, 0);
}

void ogles2_glUniform1iv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLint *value)
{
    set_uniform(ctx, location, count, K_INT, 1, 1, 0, value);
}

void ogles2_glUniform2fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLfloat *value)
{
    set_uniform(ctx, location, count, K_FLOAT, 2, 1, value, 0);
}

void ogles2_glUniform2iv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLint *value)
{
    set_uniform(ctx, location, count, K_INT, 2, 1, 0, value);
}

void ogles2_glUniform3fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLfloat *value)
{
    set_uniform(ctx, location, count, K_FLOAT, 3, 1, value, 0);
}

void ogles2_glUniform3iv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLint *value)
{
    set_uniform(ctx, location, count, K_INT, 3, 1, 0, value);
}

void ogles2_glUniform4fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLfloat *value)
{
    set_uniform(ctx, location, count, K_FLOAT, 4, 1, value, 0);
}

void ogles2_glUniform4iv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLint *value)
{
    set_uniform(ctx, location, count, K_INT, 4, 1, 0, value);
}

void ogles2_glUniform1f(struct OGLES2Context *ctx, GLint location, GLfloat v0)
{
    set_uniform(ctx, location, 1, K_FLOAT, 1, 1, &v0, 0);
}

void ogles2_glUniform2f(struct OGLES2Context *ctx, GLint location, GLfloat v0, GLfloat v1)
{
    GLfloat v[2];
    v[0] = v0; v[1] = v1;
    set_uniform(ctx, location, 1, K_FLOAT, 2, 1, v, 0);
}

void ogles2_glUniform3f(struct OGLES2Context *ctx, GLint location, GLfloat v0, GLfloat v1,
                        GLfloat v2)
{
    GLfloat v[3];
    v[0] = v0; v[1] = v1; v[2] = v2;
    set_uniform(ctx, location, 1, K_FLOAT, 3, 1, v, 0);
}

void ogles2_glUniform4f(struct OGLES2Context *ctx, GLint location, GLfloat v0, GLfloat v1,
                        GLfloat v2, GLfloat v3)
{
    GLfloat v[4];
    v[0] = v0; v[1] = v1; v[2] = v2; v[3] = v3;
    set_uniform(ctx, location, 1, K_FLOAT, 4, 1, v, 0);
}

void ogles2_glUniform1i(struct OGLES2Context *ctx, GLint location, GLint v0)
{
    set_uniform(ctx, location, 1, K_INT, 1, 1, 0, &v0);
}

void ogles2_glUniform2i(struct OGLES2Context *ctx, GLint location, GLint v0, GLint v1)
{
    GLint v[2];
    v[0] = v0; v[1] = v1;
    set_uniform(ctx, location, 1, K_INT, 2, 1, 0, v);
}

void ogles2_glUniform3i(struct OGLES2Context *ctx, GLint location, GLint v0, GLint v1, GLint v2)
{
    GLint v[3];
    v[0] = v0; v[1] = v1; v[2] = v2;
    set_uniform(ctx, location, 1, K_INT, 3, 1, 0, v);
}

void ogles2_glUniform4i(struct OGLES2Context *ctx, GLint location, GLint v0, GLint v1, GLint v2,
                        GLint v3)
{
    GLint v[4];
    v[0] = v0; v[1] = v1; v[2] = v2; v[3] = v3;
    set_uniform(ctx, location, 1, K_INT, 4, 1, 0, v);
}

void ogles2_glUniformMatrix2fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *value)
{
    if (ogles2_context_valid(ctx) && transpose != GL_FALSE) { /* GLES 2.0: must be FALSE */
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    set_uniform(ctx, location, count, K_FLOAT, 2, 2, value, 0);
}

void ogles2_glUniformMatrix3fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *value)
{
    if (ogles2_context_valid(ctx) && transpose != GL_FALSE) { /* GLES 2.0: must be FALSE */
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    set_uniform(ctx, location, count, K_FLOAT, 3, 3, value, 0);
}

void ogles2_glUniformMatrix4fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *value)
{
    if (ogles2_context_valid(ctx) && transpose != GL_FALSE) { /* GLES 2.0: must be FALSE */
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    set_uniform(ctx, location, count, K_FLOAT, 4, 4, value, 0);
}

/* glGetUniformfv / iv: the value as the first storage place holds it. */
static void get_uniform(struct OGLES2Context *ctx, GLuint program, GLint location, GLfloat *fv,
                        GLint *iv)
{
    struct OGLES2Program *p;
    const struct OGLES2ImageUniform *u;
    const struct OGLES2Place *place;
    int kind, comps, cols, c, k;
    if (!ogles2_context_valid(ctx) || !(p = linked(ctx, program)))
        return;
    if (location < 0 || location >= p->nlocations) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    u = &p->image->uniforms[p->loc_uniform[location]];
    if (!type_shape(u->type, &kind, &comps, &cols))
        return;
    if (kind == K_SAMPLER) {
        if (fv) *fv = (GLfloat)p->sampler_unit[location];
        if (iv) *iv = p->sampler_unit[location];
        return;
    }
    if (!u->nplaces)
        return;
    place = &u->places[0];
    for (c = 0; c < cols; c++)
        for (k = 0; k < comps; k++) {
            ULONG at = place->offset + (ULONG)p->loc_element[location] * place->elem_stride +
                       (ULONG)c * place->vec_stride + k;
            ULONG w = at < p->image->cbuf_words[place->stage] ? p->cbuf[place->stage][at] : 0;
            union { ULONG u; GLfloat f; } cv;
            GLfloat f;
            GLint n;
            cv.u = w;
            if (kind == K_FLOAT || place->int_as_float) {
                f = cv.f;
                n = (GLint)f;
            } else {
                n = (GLint)w;
                f = (GLfloat)n;
            }
            if (kind == K_BOOL) {
                n = w != 0;
                f = (GLfloat)n;
            }
            if (fv) fv[c * comps + k] = f;
            if (iv) iv[c * comps + k] = n;
        }
}

void ogles2_glGetUniformfv(struct OGLES2Context *ctx, GLuint program, GLint location,
                           GLfloat *params)
{
    if (params)
        get_uniform(ctx, program, location, params, 0);
}

void ogles2_glGetUniformiv(struct OGLES2Context *ctx, GLuint program, GLint location,
                           GLint *params)
{
    if (params)
        get_uniform(ctx, program, location, 0, params);
}

/* All precisions are fp32 and 32-bit ints on V3D (plan 7.8). */
void ogles2_glGetShaderPrecisionFormat(struct OGLES2Context *ctx, GLenum shadertype,
                                       GLenum precisiontype, GLint *range, GLint *precision)
{
    BOOL is_float;
    if (!ogles2_context_valid(ctx))
        return;
    if ((shadertype != GL_VERTEX_SHADER && shadertype != GL_FRAGMENT_SHADER) ||
        precisiontype < GL_LOW_FLOAT || precisiontype > GL_HIGH_INT) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    is_float = precisiontype <= GL_HIGH_FLOAT;
    if (range) {
        range[0] = is_float ? 127 : 31;
        range[1] = is_float ? 127 : 30;
    }
    if (precision)
        *precision = is_float ? 23 : 0;
}

/* No shader binary formats (GL_NUM_SHADER_BINARY_FORMATS is 0): programs
 * come from .v3dp files found by their sources' hash. */
void ogles2_glShaderBinary(struct OGLES2Context *ctx, GLsizei count, const GLuint *shaders,
                           GLenum binaryFormat, const void *binary, GLsizei length)
{
    (void)count;
    (void)shaders;
    (void)binaryFormat;
    (void)binary;
    (void)length;
    if (ogles2_context_valid(ctx))
        ogles2_error(ctx, GL_INVALID_ENUM);
}
