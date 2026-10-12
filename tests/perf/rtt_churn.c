/*
 * Where SDL's testrendercopyex spends its frame (M6, package m6-3): every
 * frame a new 640x480 texture is made a render target, the background and a
 * sprite are drawn into it, it is drawn onto the window and deleted. This
 * does the same GL calls as SDL's GLES2 renderer, in a window, and times
 * each step on the Pi's 1 MHz system timer. A second pass keeps one target
 * texture for the whole run, for comparison.
 *   usage: rtt_churn [FRAMES=120]
 * To see: the window shows a grey-blue gradient with a white square
 * moving across it, twice.
 */
#include <stdio.h>
#include <stdlib.h>
#include <proto/ogles2.h>
#pragma GCC diagnostic ignored "-Wunused-function" /* perf_target.h's gears helpers */
#include "perf_target.h"

#define CLO (*(volatile ULONG *)(0xF2000000 + 0x3004))

static ULONG now(void)
{
    ULONG v = CLO;
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char *fs_src =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

static GLuint link(void)
{
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p;
    GLint ok = 0;
    glShaderSource(v, 1, &vs_src, 0);
    glShaderSource(f, 1, &fs_src, 0);
    glCompileShader(v);
    glCompileShader(f);
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

/* A quad from (x0,y0) to (x1,y1) in clip space, the whole texture. */
static void quad(float x0, float y0, float x1, float y1)
{
    float v[16];
    v[0] = x0; v[1] = y0; v[2] = 0; v[3] = 0;
    v[4] = x1; v[5] = y0; v[6] = 1; v[7] = 0;
    v[8] = x0; v[9] = y1; v[10] = 0; v[11] = 1;
    v[12] = x1; v[13] = y1; v[14] = 1; v[15] = 1;
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static GLuint make_texture(int w, int h, int pattern)
{
    UBYTE *p = malloc((size_t)w * h * 4);
    GLuint t;
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            UBYTE *q = p + ((size_t)y * w + x) * 4;
            q[0] = pattern ? 255 : (UBYTE)(x * 128 / w + 40);
            q[1] = pattern ? 255 : (UBYTE)(y * 128 / h + 40);
            q[2] = pattern ? 255 : 160;
            q[3] = 255;
        }
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
    free(p);
    return t;
}

enum { S_CREATE, S_ATTACH, S_BG, S_SPRITE, S_UNBIND, S_SHOW, S_DELETE, S_SWAP, S_N };
static const char *names[S_N] = {
    "create texture (glTexImage2D NULL)", "bind framebuffer, attach, status",
    "draw background into it",            "draw sprite into it",
    "bind framebuffer 0",                 "draw it onto the window",
    "glDeleteTextures",                   "aglSwapBuffers",
};

static void run(GLuint fbo, GLuint bg, GLuint sprite, int frames, int churn)
{
    double sum[S_N];
    ULONG t, t1, start = 0;
    GLuint target = 0;
    int i, k, warm = 10;
    for (k = 0; k < S_N; k++)
        sum[k] = 0;
    for (i = 0; i < frames + warm; i++) {
        float sx = -0.9f + 1.6f * (float)(i % 60) / 60.0f;
        if (i == warm) {
            for (k = 0; k < S_N; k++)
                sum[k] = 0;
            start = now();
        }
        t = now();
        if (churn || !target) {
            glGenTextures(1, &target);
            glBindTexture(GL_TEXTURE_2D, target);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PG_W, PG_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        }
        t1 = now(); sum[S_CREATE] += t1 - t; t = t1;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glViewport(0, 0, PG_W, PG_H);
        t1 = now(); sum[S_ATTACH] += t1 - t; t = t1;
        glBindTexture(GL_TEXTURE_2D, bg);
        quad(-1, -1, 1, 1);
        t1 = now(); sum[S_BG] += t1 - t; t = t1;
        glBindTexture(GL_TEXTURE_2D, sprite);
        quad(sx, -0.1f, sx + 0.15f, 0.1f);
        t1 = now(); sum[S_SPRITE] += t1 - t; t = t1;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, PG_W, PG_H);
        t1 = now(); sum[S_UNBIND] += t1 - t; t = t1;
        glBindTexture(GL_TEXTURE_2D, target);
        quad(-1, -1, 1, 1);
        t1 = now(); sum[S_SHOW] += t1 - t; t = t1;
        if (churn) {
            glDeleteTextures(1, &target);
            target = 0;
        }
        t1 = now(); sum[S_DELETE] += t1 - t; t = t1;
        aglSwapBuffers();
        t1 = now(); sum[S_SWAP] += t1 - t;
    }
    t = now() - start;
    if (target)
        glDeleteTextures(1, &target);
    printf("%s, %d frames: %lu us a frame (%lu.%lu fps)\n",
           churn ? "a new target texture every frame" : "one target texture", frames,
           (unsigned long)(t / frames), (unsigned long)(frames * 1000000.0 / t),
           (unsigned long)((frames * 10000000.0 / t)) % 10);
    for (k = 0; k < S_N; k++)
        printf("  %-38s %7lu us\n", names[k], (unsigned long)(sum[k] / frames));
}

int main(int argc, char **argv)
{
    struct pg_args a;
    ULONG err = 0;
    GLuint prog, fbo, bg, sprite;
    int frames = argc > 1 ? atoi(argv[1]) : 120;
    void *ctx = 0;

    memset(&a, 0, sizeof(a));
    a.window = 1;
    if (frames < 1)
        frames = 120;
    if (!pt_open(&a, "rtt_churn: a render target a frame") || !OGLES2Open()) {
        printf("needs a window and %s\n", OGLES2NAME);
        pt_close();
        return 20;
    }
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_WINDOW, (ULONG)pt_win, OGLES2_CCT_VSYNC, FALSE, TAG_DONE);
    if (!ctx) {
        printf("aglCreateContext2 failed (%lu)\n", (unsigned long)err);
        goto out;
    }
    aglMakeCurrent(ctx);
    if (!(prog = link())) {
        printf("the program does not link\n");
        goto out;
    }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    bg = make_texture(PG_W, PG_H, 0);
    sprite = make_texture(32, 32, 1);
    glGenFramebuffers(1, &fbo);
    run(fbo, bg, sprite, frames, 1);
    run(fbo, bg, sprite, frames, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &bg);
    glDeleteTextures(1, &sprite);
    if (glGetError() != GL_NO_ERROR)
        printf("GL error\n");
out:
    if (ctx)
        aglDestroyContext(ctx);
    OGLES2Close();
    pt_close();
    return 0;
}
