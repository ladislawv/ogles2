/*
 * Mesa's V3D driver on a fake DRM device, no window system.
 *   usage: v3dglslc [-d <V3D_DEBUG flags>] [-o <dir>] vs.vert fs.frag [vs2 fs2 ...]
 *          v3dglslc SERVER   (AmigaOS: the on-target compiler, v3dp/v3dglsl_port.h)
 * Creates a V3D 4.2 screen (compat/fake_drm.c), a surfaceless GLES2 context
 * through the state tracker, and calls Mesa's GL entry points directly.
 * -o writes each program as <dir>/<vs>-<fs>.v3dp (dev/v3dp/FORMAT.md), with
 * the variants the driver compiles for draws in the default key states.
 *
 * Built for m68k-amigaos (build_m68k.sh; the seed of the S2 compiler module)
 * and natively on the PC (build_host.sh, the offline compiler), so the two can be compared
 * byte for byte.
 */
#include <stdlib.h>
#include "main/mtypes.h"
#include "main/context.h"
#include "main/shaderobj.h"
#include "api_exec_decl.h" /* generated: prototypes of the _mesa_* GL entry points */
#include "state_tracker/st_context.h"
#include "state_tracker/st_manager.h"
#include "frontend/api.h"
#include "pipe/p_screen.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "util/macros.h"
#include "v3d_screen.h"
#include "xf86drm.h"
#include "v3dp_writer.h"

#define glCreateShader       _mesa_CreateShader
#define glShaderSource       _mesa_ShaderSource
#define glCompileShader      _mesa_CompileShader
#define glGetShaderiv        _mesa_GetShaderiv
#define glGetShaderInfoLog   _mesa_GetShaderInfoLog
#define glCreateProgram      _mesa_CreateProgram
#define glAttachShader       _mesa_AttachShader
#define glLinkProgram        _mesa_LinkProgram
#define glGetProgramiv       _mesa_GetProgramiv
#define glGetProgramInfoLog  _mesa_GetProgramInfoLog
#define glDeleteShader       _mesa_DeleteShader
#define glDeleteProgram      _mesa_DeleteProgram
#define glBindAttribLocation _mesa_BindAttribLocation
#define V3DC_WRITER
static void program_path(char *out, size_t size, const char *vs_path, const char *fs_path);
static int write_program(GLuint p, const char *path, const char *vs_src, const char *fs_src);
#include "compile_common.c"
#ifdef __amigaos__
#include <proto/exec.h>
#include "v3dglsl_port.h"
#endif

int fake_drm_open(void);

#ifdef __amigaos__
void os_amiga_set_option(const char *name, const char *value);
#define set_option os_amiga_set_option
#else
static void set_option(const char *name, const char *value)
{
    setenv(name, value, 1);
}
#endif

/* The driver's driconf options, as the pipe loader passes them. */
static const driOptionDescription v3d_driconf[] = {
#include "driinfo_v3d.h"
};

/* File name part of a path, without its extension, at most 31 characters. */
static void base_name(char *out, const char *path)
{
    const char *b = path, *p, *dot = NULL;
    size_t n;
    for (p = path; *p; p++)
        if (*p == '/' || *p == ':')
            b = p + 1;
    for (p = b; *p; p++)
        if (*p == '.')
            dot = p;
    n = dot ? (size_t)(dot - b) : strlen(b);
    if (n > 31)
        n = 31;
    memcpy(out, b, n);
    out[n] = 0;
}

/* The render target of the key-state draws: an RGBA8 colour texture and a
 * 16-bit depth buffer, created once; and the textures the samplers get:
 * RGBA8, or RGBA32F for the variants that sample 32-bit returns. */
static GLuint draw_fbo, tex_2d, tex_cube, tex_2d32, tex_cube32, tex_rect, tex_rect32;

/* What write_program writes (v3dp/FORMAT.md): the variants of the default
 * key states (.v3dp); and, for a program with samplers, those of the same
 * draws with every sampler returning 32 bits (float and depth textures),
 * which ogles2.library loads only when a draw needs them (.v3dw, the same
 * file format). The command line writes both; a server request one. */
#define WRITE_BOTH 0
#define WRITE_PLAIN 1
#define WRITE_WIDE 2
static int write_mode = WRITE_BOTH;

static int make_target(void)
{
    GLuint tex, depth;
    GLenum status;

    _mesa_GenTextures(1, &tex);
    _mesa_BindTexture(GL_TEXTURE_2D, tex);
    _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    _mesa_BindTexture(GL_TEXTURE_2D, 0);
    _mesa_GenRenderbuffers(1, &depth);
    _mesa_BindRenderbuffer(GL_RENDERBUFFER, depth);
    _mesa_RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 64, 64);
    _mesa_GenFramebuffers(1, &draw_fbo);
    _mesa_BindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    _mesa_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    _mesa_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    status = _mesa_CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "v3dglslc: draw target incomplete (0x%04x)\n", (unsigned)status);
        return 0;
    }
    _mesa_Viewport(0, 0, 64, 64);
    _mesa_Enable(GL_DEPTH_TEST);

    /* Complete 1x1 RGBA8 textures for the samplers (bind_samplers). */
    {
        static const GLubyte texel[4] = { 255, 255, 255, 255 };
        GLenum face;
        _mesa_GenTextures(1, &tex_2d);
        _mesa_BindTexture(GL_TEXTURE_2D, tex_2d);
        _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_2D, 0);
        _mesa_GenTextures(1, &tex_cube);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, tex_cube);
        for (face = GL_TEXTURE_CUBE_MAP_POSITIVE_X; face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; face++)
            _mesa_TexImage2D(face, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
    /* The same as RGBA32F (OES_texture_float; nearest: not filterable), whose
     * 32-bit return sets the key's sampler_is_32b. */
    {
        static const GLfloat texel[4] = { 1, 1, 1, 1 };
        GLenum face;
        _mesa_GenTextures(1, &tex_2d32);
        _mesa_BindTexture(GL_TEXTURE_2D, tex_2d32);
        _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, texel);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_2D, 0);
        _mesa_GenTextures(1, &tex_cube32);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, tex_cube32);
        for (face = GL_TEXTURE_CUBE_MAP_POSITIVE_X; face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; face++)
            _mesa_TexImage2D(face, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, texel);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
    /* Rectangle textures (ARB_texture_rectangle, patches/0011), RGBA8 and
     * RGBA32F. */
    {
        static const GLubyte texel8[4] = { 255, 255, 255, 255 };
        static const GLfloat texel32[4] = { 1, 1, 1, 1 };
        _mesa_GenTextures(1, &tex_rect);
        _mesa_BindTexture(GL_TEXTURE_RECTANGLE, tex_rect);
        _mesa_TexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel8);
        _mesa_TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_GenTextures(1, &tex_rect32);
        _mesa_BindTexture(GL_TEXTURE_RECTANGLE, tex_rect32);
        _mesa_TexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, texel32);
        _mesa_TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_RECTANGLE, 0);
    }
    if (_mesa_GetError() != GL_NO_ERROR) {
        fprintf(stderr, "v3dglslc: the key-state textures could not be made\n");
        return 0;
    }
    return 1;
}

/* The program has a sampler uniform. */
static int has_samplers(GLuint p)
{
    GLint n = 0, i, size;
    GLenum type;
    char name[256];
    _mesa_GetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
    for (i = 0; i < n; i++) {
        _mesa_GetActiveUniform(p, i, sizeof(name), NULL, &size, &type, name);
        if (type == GL_SAMPLER_2D || type == GL_SAMPLER_CUBE || type == GL_SAMPLER_2D_RECT)
            return 1;
    }
    return 0;
}

/* The texture target and the key-state texture a sampler of `type` gets. */
static GLenum sampler_target(GLenum type, int wide, GLuint *tex)
{
    if (type == GL_SAMPLER_CUBE) {
        *tex = wide ? tex_cube32 : tex_cube;
        return GL_TEXTURE_CUBE_MAP;
    }
    if (type == GL_SAMPLER_2D_RECT) {
        *tex = wide ? tex_rect32 : tex_rect;
        return GL_TEXTURE_RECTANGLE;
    }
    *tex = wide ? tex_2d32 : tex_2d;
    return GL_TEXTURE_2D;
}

/* Give every sampler of the current program its own texture unit with a
 * complete RGBA8 texture of its kind bound, or (restore) put them all back
 * on unit 0, the value a freshly linked program has and the .v3dp template
 * records. Needed because:
 *  - samplers of different kinds on one unit make the draw an error (GLES2);
 *  - a sampler with nothing bound makes Mesa build its fallback texture,
 *    which waits on a fence (glFinish) the fake device cannot give;
 *  - the key's sampler_is_32b comes from the bound texture's format (wide:
 *    RGBA32F textures, for the variants with every sampler 32-bit). */
static void bind_samplers(GLuint p, int restore, int wide)
{
    GLint n = 0, i, j, unit = 0;

    _mesa_GetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
    for (i = 0; i < n; i++) {
        char name[256];
        GLint size, loc, units[32];
        GLenum type;
        _mesa_GetActiveUniform(p, i, sizeof(name), NULL, &size, &type, name);
        if (type != GL_SAMPLER_2D && type != GL_SAMPLER_CUBE && type != GL_SAMPLER_2D_RECT)
            continue;
        loc = _mesa_GetUniformLocation(p, name);
        if (loc < 0 || size > 32)
            continue;
        for (j = 0; j < size; j++) {
            units[j] = restore ? 0 : unit;
            if (!restore) {
                GLuint tex;
                GLenum target = sampler_target(type, wide, &tex);
                _mesa_ActiveTexture(GL_TEXTURE0 + unit);
                _mesa_BindTexture(target, tex);
                unit++;
            }
        }
        _mesa_Uniform1iv(loc, size, units);
    }
    _mesa_ActiveTexture(GL_TEXTURE0);
}

/* Draw once per default key state (plan section 8.2) so the driver compiles
 * the variants real draws use: triangles, lines and points into an RGBA8
 * target with depth on. The fake device runs nothing; the draws only make
 * the driver compute its keys and compile. */
static int draw_key_states(GLuint p, int wide)
{
    static const GLenum modes[] = { GL_TRIANGLES, GL_LINES, GL_POINTS };
    static const GLfloat zeros[4 * 4 * 3]; /* 3 vertices of a mat4 attribute */
    GLint n = 0, i, err;
    unsigned m;

    if (!draw_fbo && !make_target())
        return 0;
    _mesa_UseProgram(p);
    bind_samplers(p, 0, wide);
    _mesa_GetProgramiv(p, GL_ACTIVE_ATTRIBUTES, &n);
    for (i = 0; i < n; i++) {
        char name[256];
        GLint size, loc, col, cols;
        GLenum type;
        _mesa_GetActiveAttrib(p, i, sizeof(name), NULL, &size, &type, name);
        loc = _mesa_GetAttribLocation(p, name);
        if (loc < 0)
            continue; /* built-in input such as gl_VertexID */
        cols = type == GL_FLOAT_MAT2 ? 2 : type == GL_FLOAT_MAT3 ? 3 : type == GL_FLOAT_MAT4 ? 4 : 1;
        for (col = 0; col < cols; col++) {
            _mesa_EnableVertexAttribArray(loc + col);
            _mesa_VertexAttribPointer(loc + col, 4, GL_FLOAT, GL_FALSE, 0, zeros);
        }
    }
    for (m = 0; m < ARRAY_SIZE(modes); m++)
        _mesa_DrawArrays(modes[m], 0, 3);
    /* Submit the job (a no-op on the fake device) so jobs do not pile up.
     * Not glFinish: its fence needs a sync file the fake device cannot give. */
    _mesa_Flush();
    for (i = 0; i < 16; i++)
        _mesa_DisableVertexAttribArray(i);
    bind_samplers(p, 1, 0);
    _mesa_UseProgram(0);
    err = _mesa_GetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "v3dglslc: GL error 0x%04x in the key-state draws\n", (unsigned)err);
        return 0;
    }
    return 1;
}

/* -o: <dir>/<vs>-<fs>.v3dp. */
static void program_path(char *out, size_t size, const char *vs_path, const char *fs_path)
{
    char vs_base[32], fs_base[32];
    size_t n = strlen(out_dir);
    base_name(vs_base, vs_path);
    base_name(fs_base, fs_path);
    snprintf(out, size, "%s%s%s-%s.v3dp", out_dir,
             n && out_dir[n - 1] != '/' && out_dir[n - 1] != ':' ? "/" : "", vs_base, fs_base);
}

static int write_program(GLuint p, const char *path, const char *vs_src, const char *fs_src)
{
    GET_CURRENT_CONTEXT(ctx);
    struct gl_shader_program *prog = _mesa_lookup_shader_program(ctx, p);
    /* The bindings of a V3DGLSL_LINK_BOUND request are hashed as a third
     * "source" (v3dp/FORMAT.md, PROGRAM). */
    static const unsigned stages[3] = { MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT, V3DP_HASH_BINDINGS };
    const char *sources[3];
    unsigned nsrc = link_bindings ? 3 : 2;
    int drawn;

    if (!prog)
        return 0;
    sources[0] = vs_src;
    sources[1] = fs_src;
    sources[2] = link_bindings;
    if (write_mode != WRITE_WIDE) {
        v3dp_capture_begin();
        drawn = draw_key_states(p, 0);
        v3dp_capture_end();
        if (!drawn || v3dp_write(path, prog, nsrc, stages, sources) != 0)
            return 0;
    }
    /* The 32-bit-sampler variants. After the plain draws the driver
     * compiles only the fragment shaders again (the vertex shaders' keys are
     * the same), so a .v3dw may hold fragment variants only: the runtime
     * takes only those from it. The command line's goes next to the .v3dp. */
    if (write_mode != WRITE_PLAIN && has_samplers(p)) {
        char wpath[512];
        size_t n = strlen(path);
        if (write_mode == WRITE_WIDE)
            snprintf(wpath, sizeof(wpath), "%s", path);
        else if (n > 5 && !strcmp(path + n - 5, ".v3dp") && n < sizeof(wpath))
            snprintf(wpath, sizeof(wpath), "%.*s.v3dw", (int)(n - 5), path);
        else
            return 0;
        v3dp_capture_begin();
        drawn = draw_key_states(p, 1);
        v3dp_capture_end();
        if (!drawn || v3dp_write(wpath, prog, nsrc, stages, sources) != 0)
            return 0;
    } else if (write_mode == WRITE_WIDE) {
        return 0; /* nothing samples: no such file */
    }
    return 1;
}

#ifdef __amigaos__
/* A request goes back to its sender; one without a reply port (a QUIT sent
 * from ogles2.library's expunge, which cannot wait) is ours to free. */
static void done(struct V3DGLSLRequest *r)
{
    if (r->msg.mn_ReplyPort)
        ReplyMsg(&r->msg);
    else
        FreeVec(r);
}

/* The on-target compiler (plan 8.4): serve V3DGLSL_LINK requests on a
 * public port until V3DGLSL_QUIT. One context for all of them; each request
 * deletes its GL objects again. A second server started by a race finds the
 * port taken and leaves. */
static int serve(void)
{
    struct MsgPort *port;
    struct V3DGLSLRequest *r;
    int quit = 0, served = 0;

    Forbid();
    if (FindPort((CONST_STRPTR)V3DGLSL_PORT_NAME)) {
        Permit();
        fprintf(stderr, "v3dglslc: a compiler server is already running\n");
        return 0;
    }
    if (!(port = CreateMsgPort())) {
        Permit();
        return 20;
    }
    port->mp_Node.ln_Name = (char *)V3DGLSL_PORT_NAME;
    port->mp_Node.ln_Pri = 0;
    AddPort(port);
    Permit();
    fprintf(stderr, "v3dglslc: serving on \"%s\"\n", V3DGLSL_PORT_NAME);

    while (!quit) {
        WaitPort(port);
        while ((r = (struct V3DGLSLRequest *)GetMsg(port))) {
            if (r->log && r->log_size)
                r->log[0] = 0;
            if (r->version != V3DGLSL_VERSION ||
                (r->op != V3DGLSL_LINK && r->op != V3DGLSL_QUIT && r->op != V3DGLSL_COMPILE &&
                 r->op != V3DGLSL_LINK32 && r->op != V3DGLSL_LINK_BOUND) ||
                ((r->op == V3DGLSL_LINK || r->op == V3DGLSL_LINK32 || r->op == V3DGLSL_LINK_BOUND) &&
                 (!r->vs || !r->fs || !r->out_path)) ||
                (r->op == V3DGLSL_LINK_BOUND && !r->bindings) ||
                (r->op == V3DGLSL_COMPILE && !r->vs == !r->fs)) {
                log_add(r->log, r->log_size, "the compiler does not understand this request", NULL);
                r->result = 3;
            } else if (r->op == V3DGLSL_QUIT) {
                r->result = 0;
                quit = 1;
            } else if (r->op == V3DGLSL_COMPILE) {
                /* The front end only: GL_COMPILE_STATUS and the shader's info log. */
                int ok;
                GLuint s = compile_src(r->vs ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER,
                                       r->vs ? "vertex shader" : "fragment shader",
                                       r->vs ? r->vs : r->fs, &ok, NULL, 0);
                char info[2048];
                info[0] = 0;
                glGetShaderInfoLog(s, sizeof(info), NULL, info);
                log_add(r->log, r->log_size, info, NULL);
                glDeleteShader(s);
                r->result = ok ? 0 : 1;
            } else {
                write_mode = r->op == V3DGLSL_LINK32 ? WRITE_WIDE : WRITE_PLAIN;
                link_bindings = r->op == V3DGLSL_LINK_BOUND ? r->bindings : NULL;
                r->result = link_pair("vertex shader", "fragment shader", r->vs, r->fs, r->out_path,
                                      r->log, r->log_size) ? 0 : 1;
                link_bindings = NULL;
                served++;
            }
            done(r);
        }
    }
    /* Gone from the list first, so nobody queues more; then fail what came. */
    Forbid();
    RemPort(port);
    Permit();
    while ((r = (struct V3DGLSLRequest *)GetMsg(port))) {
        log_add(r->log, r->log_size, "the compiler is shutting down", NULL);
        r->result = 2;
        done(r);
    }
    DeleteMsgPort(port);
    fprintf(stderr, "v3dglslc: server done, %d program(s)\n", served);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    int first = 1;
    const char *debug = NULL;
    char debug_flags[256];
    driOptionCache info, cache;
    struct pipe_screen_config config;
    struct pipe_screen *screen;
    struct pipe_frontend_screen fscreen;
    struct st_context_attribs attribs;
    enum st_context_error err = ST_CONTEXT_SUCCESS;
    struct st_context *st;

#ifdef __amigaos__
    /* libnix sends stderr to the console, not into the shell's ">file"
     * redirection (seen on the A1200; vamos merges them). Everything here and
     * Mesa's own log (the QPU listing) is written to stderr, so make it the
     * stdout stream: one ordered stream that the redirection captures. */
    __sF[2] = __sF[1];
    /* Line-buffered, so a failed assertion's message (abort() skips the
     * final flush) and everything before it reach the file. */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
#else
    /* Every compile must reach the driver (and the .v3dp hook). */
    set_option("MESA_SHADER_CACHE_DISABLE", "true");
#endif

#ifdef __amigaos__
    int server = argc == 2 && !strcmp(argv[1], "SERVER");
#endif
    while (first + 1 < argc && argv[first][0] == '-') {
        if (!strcmp(argv[first], "-d"))
            debug = argv[first + 1];
        else if (!strcmp(argv[first], "-o"))
            out_dir = argv[first + 1];
        else
            break;
        first += 2;
    }
    if (debug) {
        snprintf(debug_flags, sizeof(debug_flags), "%s", debug);
        set_option("V3D_DEBUG", debug_flags);
    }

    memset(&info, 0, sizeof(info));
    memset(&cache, 0, sizeof(cache));
    driParseOptionInfo(&info, v3d_driconf, ARRAY_SIZE(v3d_driconf));
    memset(&config, 0, sizeof(config));
    config.options = &cache;
    config.options_info = &info;

    screen = v3d_screen_create(fake_drm_open(), &config, NULL);
    if (!screen) {
        fprintf(stderr, "v3dglslc: v3d_screen_create failed\n");
        return 20;
    }

    memset(&fscreen, 0, sizeof(fscreen));
    fscreen.screen = screen;
    memset(&attribs, 0, sizeof(attribs));
    attribs.profile = API_OPENGLES2;
    attribs.major = 2;
    attribs.minor = 0;
    st = st_api_create_context(&fscreen, &attribs, &err, NULL);
    if (!st) {
        fprintf(stderr, "v3dglslc: st_api_create_context failed (%d)\n", (int)err);
        return 20;
    }
    if (!st_api_make_current(st, NULL, NULL)) {
        fprintf(stderr, "v3dglslc: st_api_make_current failed\n");
        return 20;
    }
    /* ogles2.library's limits, which its GL queries report too (GLSL's
     * gl_Max* constants must equal them: dEQP-GLES2 builtin_variable): one
     * draw buffer (no EXT_draw_buffers: gl_FragData[1] does not compile), 16
     * texture units in each stage and in all (OGLES2_MAX_TEXTURE_UNITS; the
     * driver offers 24). */
    {
        GET_CURRENT_CONTEXT(ctx);
        ctx->Const.MaxDrawBuffers = 1;
        ctx->Const.Program[MESA_SHADER_VERTEX].MaxTextureImageUnits = 16;
        ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits = 16;
        ctx->Const.MaxCombinedTextureImageUnits = 16;
    }
    fprintf(stderr, "v3dglslc: renderer %s\n", (const char *)_mesa_GetString(GL_RENDERER));
#ifdef __amigaos__
    if (server)
        return serve();
#endif
    return run_pairs(argc, argv, first);
}
