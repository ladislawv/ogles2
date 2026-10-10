/*
 * Mesa's V3D driver on a fake DRM device, no window system.
 *   usage: v3dglslc [-d <V3D_DEBUG flags>] [-o <dir>] vs.vert fs.frag [vs2 fs2 ...]
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
#define V3DC_WRITER
static int write_program(GLuint p, const char *vs_path, const char *fs_path,
                         const char *vs_src, const char *fs_src);
#include "compile_common.c"

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
 * 16-bit depth buffer, created once. */
static GLuint draw_fbo, tex_2d, tex_cube;

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
    return 1;
}

/* Give every sampler of the current program its own texture unit with a
 * complete RGBA8 texture of its kind bound, or (restore) put them all back
 * on unit 0, the value a freshly linked program has and the .v3dp template
 * records. Needed because:
 *  - samplers of different kinds on one unit make the draw an error (GLES2);
 *  - a sampler with nothing bound makes Mesa build its fallback texture,
 *    which waits on a fence (glFinish) the fake device cannot give;
 *  - the key's sampler_is_32b comes from the bound texture's format. */
static void bind_samplers(GLuint p, int restore)
{
    GLint n = 0, i, j, unit = 0;

    _mesa_GetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
    for (i = 0; i < n; i++) {
        char name[256];
        GLint size, loc, units[32];
        GLenum type;
        _mesa_GetActiveUniform(p, i, sizeof(name), NULL, &size, &type, name);
        if (type != GL_SAMPLER_2D && type != GL_SAMPLER_CUBE)
            continue;
        loc = _mesa_GetUniformLocation(p, name);
        if (loc < 0 || size > 32)
            continue;
        for (j = 0; j < size; j++) {
            units[j] = restore ? 0 : unit;
            if (!restore) {
                _mesa_ActiveTexture(GL_TEXTURE0 + unit);
                _mesa_BindTexture(type == GL_SAMPLER_2D ? GL_TEXTURE_2D : GL_TEXTURE_CUBE_MAP,
                                  type == GL_SAMPLER_2D ? tex_2d : tex_cube);
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
static int draw_key_states(GLuint p)
{
    static const GLenum modes[] = { GL_TRIANGLES, GL_LINES, GL_POINTS };
    static const GLfloat zeros[4 * 4 * 3]; /* 3 vertices of a mat4 attribute */
    GLint n = 0, i, err;
    unsigned m;

    if (!draw_fbo && !make_target())
        return 0;
    _mesa_UseProgram(p);
    bind_samplers(p, 0);
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
    bind_samplers(p, 1);
    _mesa_UseProgram(0);
    err = _mesa_GetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "v3dglslc: GL error 0x%04x in the key-state draws\n", (unsigned)err);
        return 0;
    }
    return 1;
}

static int write_program(GLuint p, const char *vs_path, const char *fs_path,
                         const char *vs_src, const char *fs_src)
{
    GET_CURRENT_CONTEXT(ctx);
    struct gl_shader_program *prog = _mesa_lookup_shader_program(ctx, p);
    static const unsigned stages[2] = { MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT };
    const char *sources[2];
    char vs_base[32], fs_base[32], path[512];
    size_t n = strlen(out_dir);
    int drawn;

    if (!prog)
        return 0;
    v3dp_capture_begin();
    drawn = draw_key_states(p);
    v3dp_capture_end();
    if (!drawn)
        return 0;
    sources[0] = vs_src;
    sources[1] = fs_src;
    base_name(vs_base, vs_path);
    base_name(fs_base, fs_path);
    snprintf(path, sizeof(path), "%s%s%s-%s.v3dp", out_dir,
             n && out_dir[n - 1] != '/' && out_dir[n - 1] != ':' ? "/" : "", vs_base, fs_base);
    return v3dp_write(path, prog, 2, stages, sources) == 0;
}

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
    fprintf(stderr, "v3dglslc: renderer %s\n", (const char *)_mesa_GetString(GL_RENDERER));
    return run_pairs(argc, argv, first);
}
