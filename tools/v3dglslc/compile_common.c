/*
 * Shared by v3dglslc.c (m68k or x86-64, Mesa entry points called directly) and
 * s1ref.c (x86-64, EGL + libGLESv2 under Mesa's drm-shim): the same GLES2
 * calls on the same shader files, so the V3D driver sees the same work.
 *
 * Everything is printed to stderr, where the V3D driver prints its QPU code
 * (V3D_DEBUG=precompile,qpu), so the two land in one ordered stream; AmigaOS
 * merges stderr into stdout anyway.
 *
 * The includer defines the gl* names (directly or via the Khronos headers).
 * With V3DC_WRITER it also provides write_program(), called after each
 * successful link while -o is given.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef V3DC_WRITER
static const char *out_dir; /* -o: write .v3dp files here */
#endif

static char *load_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n + 1);
    if (buf) {
        n = (long)fread(buf, 1, n, f);
        buf[n] = 0;
    }
    fclose(f);
    return buf;
}

/* Returns the shader; *src_out gets its source text, which the caller frees. */
static GLuint compile_one(GLenum type, const char *path, int *ok_out, char **src_out)
{
    char *src = load_text(path);
    const GLchar *srcs[1];
    GLuint s;
    GLint ok = 0;
    char log[2048];

    *src_out = src;
    if (!src) {
        fprintf(stderr, "v3dglslc: cannot read %s\n", path);
        *ok_out = 0;
        return 0;
    }
    srcs[0] = src;
    s = glCreateShader(type);
    glShaderSource(s, 1, srcs, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    log[0] = 0;
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    fprintf(stderr, "v3dglslc: compile %s: %s%s%s\n", path, ok ? "ok" : "FAILED",
            log[0] ? "\n" : "", log);
    *ok_out = ok;
    return s;
}

/* Compile and link one vertex+fragment pair. With V3D_DEBUG=precompile the
 * driver compiles its default variants at link time and prints them. */
static int compile_link(const char *vs_path, const char *fs_path)
{
    GLuint p, vs, fs;
    GLint ok = 0;
    int vs_ok, fs_ok;
    char log[2048];
    char *vs_src, *fs_src;

    fprintf(stderr, "v3dglslc: ==== program %s + %s\n", vs_path, fs_path);
    vs = compile_one(GL_VERTEX_SHADER, vs_path, &vs_ok, &vs_src);
    fs = compile_one(GL_FRAGMENT_SHADER, fs_path, &fs_ok, &fs_src);
    if (!vs_ok || !fs_ok) {
        free(vs_src);
        free(fs_src);
        return 0;
    }
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    log[0] = 0;
    glGetProgramInfoLog(p, sizeof(log), NULL, log);
    fprintf(stderr, "v3dglslc: link: %s%s%s\n", ok ? "ok" : "FAILED", log[0] ? "\n" : "", log);
#ifdef V3DC_WRITER
    if (ok && out_dir)
        ok = write_program(p, vs_path, fs_path, vs_src, fs_src);
#endif
    free(vs_src);
    free(fs_src);
    return ok;
}

static int run_pairs(int argc, char **argv, int first)
{
    int i, failed = 0;
    for (i = first; i + 1 < argc; i += 2)
        if (!compile_link(argv[i], argv[i + 1]))
            failed++;
    fprintf(stderr, "v3dglslc: done, %d program(s) failed\n", failed);
    return failed ? 10 : 0;
}
