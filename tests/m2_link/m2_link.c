/*
 * M2 test: linking from program binaries, attribute and uniform locations,
 * glUniform* and glGetUniform*, on a context without a surface (runs under
 * vamos as well as on the A1200).
 *   usage: m2_link [NOGPU] [DIR]   (DIR: the prefix of shaders/, e.g. work:)
 *
 * The programs' GLSL is read from shaders/ in the current directory, their
 * binaries (<hash>.v3dp, make test-v3dp) come from PROGDIR: or OGLES2Cache:.
 *   smooth  tests/common/smooth_shaders.h (written out as shaders/smooth.*)
 *   light   mat3, vec3 arrays and an int (tools/v3dglslc/test/shaders)
 *   state, tex  samplers (TMU_CONFIG recipes), state variables
 *   skin    an indirectly indexed uniform array (UBO_ADDR)
 *   a program without a binary fails to link and lands in
 *           T:ogles2_missing_shaders.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ogles2.h>
#include "../common/smooth_shaders.h"

static int failures, checks;
static const char *dir = ""; /* prefix of shaders/ */

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *s;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    s = malloc(n + 1);
    if (s && fread(s, 1, n, f) != (size_t)n) {
        free(s);
        s = 0;
    }
    if (s)
        s[n] = 0;
    fclose(f);
    return s;
}

/* Links vs + fs; returns the program, its link status in *ok, the log printed. */
static GLuint link(const char *name, const char *vs_src, const char *fs_src, GLint *ok)
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER), p;
    char log[300];
    GLsizei n = 0;
    glShaderSource(vs, 1, &vs_src, 0);
    glShaderSource(fs, 1, &fs_src, 0);
    glCompileShader(vs);
    glCompileShader(fs);
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(p, GL_LINK_STATUS, ok);
    glGetProgramInfoLog(p, sizeof(log), &n, log);
    printf("     %s: link %s%s%s\n", name, *ok ? "ok" : "failed", n ? ": " : "", n ? log : "");
    return p;
}

static GLuint link_files(const char *name, GLint *ok)
{
    char path[160];
    char *vs, *fs;
    GLuint p;
    sprintf(path, "%sshaders/%s.vert", dir, name);
    vs = read_text(path);
    sprintf(path, "%sshaders/%s.frag", dir, name);
    fs = read_text(path);
    if (!vs || !fs) {
        printf("     %s: cannot read shaders/%s.vert or .frag\n", name, name);
        *ok = 0;
        return 0;
    }
    p = link(name, vs, fs, ok);
    free(vs);
    free(fs);
    return p;
}

static void test_smooth(void)
{
    static const GLfloat m[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    GLfloat back[16];
    GLint ok, n, i, size;
    GLenum type;
    char name[40];
    GLuint p = link("smooth", smooth_vs, smooth_fs, &ok);
    check(ok, "smooth: links from its binary");
    if (!ok)
        return;
    glGetProgramiv(p, GL_ACTIVE_ATTRIBUTES, &n);
    check(n == 2, "smooth: 2 active attributes");
    glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
    check(n == 1, "smooth: 1 active uniform");
    glGetActiveUniform(p, 0, sizeof(name), 0, &size, &type, name);
    check(!strcmp(name, "u_mvp") && size == 1 && type == GL_FLOAT_MAT4, "smooth: u_mvp is a mat4");
    check(glGetAttribLocation(p, "a_position") >= 0 && glGetAttribLocation(p, "a_color") >= 0 &&
          glGetAttribLocation(p, "a_position") != glGetAttribLocation(p, "a_color") &&
          glGetAttribLocation(p, "nothing") == -1, "smooth: attribute locations");
    i = glGetUniformLocation(p, "u_mvp");
    check(i == 0 && glGetUniformLocation(p, "u_mvp[0]") == -1 && glGetUniformLocation(p, "x") == -1,
          "smooth: uniform locations");
    glUseProgram(p);
    glUniformMatrix4fv(i, 1, GL_FALSE, m);
    glGetUniformfv(p, i, back);
    check(!memcmp(m, back, sizeof(m)), "smooth: glUniformMatrix4fv reads back");
    glUniformMatrix4fv(i, 1, GL_TRUE, m);
    check(glGetError() == GL_INVALID_VALUE, "smooth: transpose TRUE is GL_INVALID_VALUE");
    glUniform4f(i, 1, 2, 3, 4);
    check(glGetError() == GL_INVALID_OPERATION, "smooth: glUniform4f on a mat4 is GL_INVALID_OPERATION");
    glUniform1f(-1, 5.0f);
    check(glGetError() == GL_NO_ERROR, "smooth: location -1 is ignored");
    glUseProgram(0);
    glDeleteProgram(p);
}

static void test_light(void)
{
    static const GLfloat nm[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    GLfloat back[16];
    GLint ok, loc, iv[1], size, n;
    GLenum type;
    char name[40];
    GLuint p = link_files("light", &ok);
    check(ok, "light: links from its binary");
    if (!ok)
        return;
    glUseProgram(p);
    loc = glGetUniformLocation(p, "normal_matrix");
    glUniformMatrix3fv(loc, 1, GL_FALSE, nm);
    memset(back, 0, sizeof(back));
    glGetUniformfv(p, loc, back);
    check(loc >= 0 && !memcmp(nm, back, sizeof(nm)), "light: mat3 (columns 4 words apart) reads back");
    loc = glGetUniformLocation(p, "light_dir");
    check(loc >= 0 && glGetUniformLocation(p, "light_dir[0]") == loc &&
          glGetUniformLocation(p, "light_dir[3]") == loc + 3 &&
          glGetUniformLocation(p, "light_dir[4]") == -1, "light: array element locations");
    glUniform3f(loc + 2, 0.5f, 0.25f, 0.125f);
    glGetUniformfv(p, loc + 2, back);
    check(back[0] == 0.5f && back[1] == 0.25f && back[2] == 0.125f, "light: light_dir[2] reads back");
    {
        static const GLfloat two[6] = {9, 9, 9, 8, 8, 8};
        glUniform3fv(loc + 3, 2, two); /* the second runs past the end: ignored */
        glGetUniformfv(p, loc + 3, back);
        check(glGetError() == GL_NO_ERROR && back[0] == 9.0f, "light: a count past the array's end is cut");
    }
    loc = glGetUniformLocation(p, "light_count");
    glUniform1i(loc, 3);
    glGetUniformiv(p, loc, iv);
    check(iv[0] == 3, "light: int uniform reads back");
    glUniform1f(loc, 3.0f);
    check(glGetError() == GL_INVALID_OPERATION, "light: glUniform1f on an int is GL_INVALID_OPERATION");
    glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
    check(n == 5, "light: 5 active uniforms");
    for (loc = 0; loc < n; loc++) {
        glGetActiveUniform(p, (GLuint)loc, sizeof(name), 0, &size, &type, name);
        if (!strcmp(name, "light_dir[0]"))
            break;
    }
    check(loc < n && size == 4 && type == GL_FLOAT_VEC3, "light: glGetActiveUniform names light_dir[0], size 4");
    glUseProgram(0);
    glDeleteProgram(p);
}

static void test_links(const char *name)
{
    char what[120];
    GLint ok;
    GLuint p = link_files(name, &ok);
    sprintf(what, "%s: links from its binary", name);
    check(ok, what);
    glDeleteProgram(p);
}

static void test_missing(void)
{
    GLint ok;
    BPTR l;
    char vs[600];
    GLuint p;
    sprintf(vs, "%s// a comment no binary has\n", smooth_vs);
    DeleteFile((STRPTR) "T:ogles2_missing_shaders.txt");
    p = link("smooth+comment", vs, smooth_fs, &ok);
    check(!ok, "a program without a binary fails to link");
    l = Lock((STRPTR) "T:ogles2_missing_shaders.txt", ACCESS_READ);
    check(l != 0, "its sources are in T:ogles2_missing_shaders.txt");
    if (l)
        UnLock(l);
    glDeleteProgram(p);
}

int main(int argc, char **argv)
{
    ULONG err = 0;
    void *ctx;
    int i;
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "NOGPU"))
            dir = argv[i];
    printf("ogles2 M2: linking from program binaries, uniforms\n");
    if (!OGLES2Open()) {
        printf("cannot open %s\n", OGLES2NAME);
        return 20;
    }
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_PISTORM_STATE_ONLY, TRUE, TAG_DONE);
    if (!ctx) {
        printf("FAIL: create a state-only context (error %lu)\n", (unsigned long)err);
        return 20;
    }
    aglMakeCurrent(ctx);
    test_smooth();
    test_light();
    test_links("skin");
    {
        /* Again: the context's program cache serves it (the log shows one load). */
        GLint ok;
        GLuint p = link("smooth again", smooth_vs, smooth_fs, &ok);
        check(ok, "smooth: links a second time (program cache)");
        glDeleteProgram(p);
    }
    test_links("state");
    test_links("tex");
    test_missing();
    check(glGetError() == GL_NO_ERROR, "no GL error left over");
    aglDestroyContext(ctx);
    OGLES2Close();
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
