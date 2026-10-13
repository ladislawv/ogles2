/*
 * M6 test: the TFU (texture formatting unit) for glGenerateMipmap and for
 * uploads, on a hidden 256x128 bitmap. Run it as it is (the TFU makes the
 * levels and tiles whole uploads of 128x128 and up) and with
 * ENV:OGLES2/Probe MU (the CPU does both): both pass, and the timings
 * compare them. Nothing to watch.
 *   usage: m6_tfu
 *   upload   256x256 RGBA8 noise: the first glGenerateMipmap lays the chain
 *            out (the CPU's), a new level 0 then goes to the TFU; every
 *            level drawn 1:1 (base = max level, SGIS_texture_lod) against
 *            a 2x2 box filter computed here
 *   npot     200x120: levels 100x60 .. 1x1, a smooth gradient
 *   target   a 512x512 render target (glmark2's refract's case): four
 *            colours drawn, mipmapped, each level's quadrants; then 20
 *            frames of draw + glGenerateMipmap, timed
 *   formats  RGB565 and RGBA4 (16-bit storage), one colour through the chain
 *   tfu upload  512x512 and 600x520 RGBA8, 256x128 RGB565: drawn 1:1,
 *            exact; a box the CPU stores over a TFU upload
 *   upload time  glTexImage2D and glTexSubImage2D, 256x256 and 1024x1024
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#include <proto/ogles2.h>
#include "../common/smooth_shaders.h"
#include "../common/tex_shaders.h"

#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

struct GfxBase *GfxBase;
struct Device *TimerBase;
static struct timerequest treq;
static int failures, checks;
static GLuint smooth, tex1;
#define W 256
#define H 128

static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

/* Microseconds, from the E-clock. */
static ULONG now_us(void)
{
    struct EClockVal ev;
    ULONG f = ReadEClock(&ev);
    return (ULONG)(((unsigned long long)ev.ev_hi << 32 | ev.ev_lo) * 1000000ULL / f);
}

static GLuint link(const char *vs, const char *fs, const char *a0, const char *a1)
{
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p;
    GLint ok = 0;
    glShaderSource(v, 1, &vs, 0);
    glShaderSource(f, 1, &fs, 0);
    glCompileShader(v);
    glCompileShader(f);
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, a0);
    glBindAttribLocation(p, 1, a1);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

/* A rectangle x0..x1, y0..y1 (NDC), one colour (smooth program). */
static void rect(float x0, float x1, float y0, float y1, float r, float g, float b)
{
    GLfloat v[12];
    v[0] = x0; v[1] = y0; v[2] = 0;
    v[3] = x1; v[4] = y0; v[5] = 0;
    v[6] = x0; v[7] = y1; v[8] = 0;
    v[9] = x1; v[10] = y1; v[11] = 0;
    glUseProgram(smooth);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glVertexAttrib4f(1, r, g, b, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Level l of texture t drawn 1:1 at the screen's corner and read back as
 * RGBA rows, bottom up (texel rows, as uploaded). */
static void draw_level(GLuint t, int l, int w, int h, UBYTE *out)
{
    static const GLfloat v[] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1, 1, 1, 0, 1, 1};
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, l);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, l);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(tex1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, v + 3);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    glViewport(0, 0, W, H);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
}

/* The next level: each texel the mean of a 2x2 box (an edge of one texel
 * reuses its row or column), rounded -- the CPU path's filter. */
static void box_down(const UBYTE *src, int sw, int sh, UBYTE *dst, int dw, int dh)
{
    int x, y, c;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            int x0 = x * 2, y0 = y * 2;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
            for (c = 0; c < 4; c++)
                dst[(y * dw + x) * 4 + c] = (UBYTE)((src[(y0 * sw + x0) * 4 + c] + src[(y0 * sw + x1) * 4 + c] +
                                                     src[(y1 * sw + x0) * 4 + c] + src[(y1 * sw + x1) * 4 + c] + 2) / 4);
        }
}

/* Every level 1.. of texture t (level 0 `img`, w x h, as uploaded) against
 * the box filter: the largest channel difference of each level within
 * 2 + its number (each level's rounding may differ from the one before). */
static void compare_levels(GLuint t, const UBYTE *img, int w, int h, const char *name)
{
    UBYTE *a = malloc((size_t)w * h * 4), *b = malloc((size_t)w * h * 4), *got = malloc((size_t)w * h * 4);
    int l = 0, lw = w, lh = h, worst_level = -1, worst = 0, ok = 1;
    char msg[160];
    if (!a || !b || !got) {
        check(0, "memory for the comparison");
        free(a); free(b); free(got);
        return;
    }
    memcpy(a, img, (size_t)w * h * 4);
    while (lw > 1 || lh > 1) {
        int nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1, i, d = 0;
        UBYTE *tmp;
        box_down(a, lw, lh, b, nw, nh);
        l++;
        if (nw <= W && nh <= H) {
            draw_level(t, l, nw, nh, got);
            for (i = 0; i < nw * nh * 4; i++) {
                int e = got[i] - b[i];
                if (e < 0)
                    e = -e;
                if ((i & 3) == 3)
                    continue; /* the bitmap's alpha is not the texture's */
                if (e > d)
                    d = e;
            }
            if (d > 2 + l)
                ok = 0;
            if (d > worst) {
                worst = d;
                worst_level = l;
            }
        }
        tmp = a; a = b; b = tmp;
        lw = nw;
        lh = nh;
    }
    sprintf(msg, "%s: %d levels against a 2x2 box filter, largest difference %d (level %d)", name, l, worst,
            worst_level);
    check(ok && glGetError() == GL_NO_ERROR, msg);
    free(a);
    free(b);
    free(got);
}

static GLuint texture(int w, int h, GLenum format, GLenum type, const void *data)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format, type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static ULONG noise(ULONG i)
{
    i ^= i >> 7;
    i *= 0x9E3779B1UL;
    return i ^ (i >> 15);
}

static void test_upload(void)
{
    UBYTE *img = malloc(256 * 256 * 4);
    GLuint t;
    ULONG t0, t1;
    int i;
    char msg[120];
    if (!img)
        return;
    for (i = 0; i < 256 * 256 * 4; i++)
        img[i] = (UBYTE)noise((ULONG)i);
    t = texture(256, 256, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D); /* lays the chain out */
    compare_levels(t, img, 256, 256, "upload, first glGenerateMipmap");
    for (i = 0; i < 256 * 256 * 4; i++)
        img[i] = (UBYTE)noise((ULONG)i + 12345);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, img);
    t0 = now_us();
    glGenerateMipmap(GL_TEXTURE_2D);
    glFinish();
    t1 = now_us();
    compare_levels(t, img, 256, 256, "upload, new level 0, glGenerateMipmap again");
    sprintf(msg, "upload: glGenerateMipmap of 256x256 took %lu us", (unsigned long)(t1 - t0));
    check(1, msg);
    glDeleteTextures(1, &t);
    free(img);
}

static void test_npot(void)
{
    UBYTE *img = malloc(200 * 120 * 4);
    GLuint t;
    int x, y, round;
    if (!img)
        return;
    for (round = 0; round < 2; round++) {
        for (y = 0; y < 120; y++)
            for (x = 0; x < 200; x++) {
                UBYTE *p = img + (y * 200 + x) * 4;
                p[0] = (UBYTE)(round ? 255 - x : x);
                p[1] = (UBYTE)(y * 2);
                p[2] = (UBYTE)((x + y) / 2);
                p[3] = 255;
            }
        if (!round) {
            t = texture(200, 120, GL_RGBA, GL_UNSIGNED_BYTE, img);
        } else {
            glBindTexture(GL_TEXTURE_2D, t);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 200, 120, GL_RGBA, GL_UNSIGNED_BYTE, img);
        }
        glGenerateMipmap(GL_TEXTURE_2D);
        compare_levels(t, img, 200, 120, round ? "npot 200x120, again" : "npot 200x120, first");
    }
    glDeleteTextures(1, &t);
    free(img);
}

/* Four colours in the quadrants of the bound framebuffer (512x512). */
static void quadrants(float shift)
{
    glViewport(0, 0, 512, 512);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    rect(-1, 0, -1, 0, 1, shift, 0);
    rect(0, 1, -1, 0, 0, 1, shift);
    rect(-1, 0, 0, 1, shift, 0, 1);
    rect(0, 1, 0, 1, 1, 1, 1 - shift);
}

static int near(UBYTE v, float want, int tol)
{
    int w = (int)(want * 255.0f + 0.5f), d = (int)v - w;
    return d >= -tol && d <= tol;
}

static void test_target(void)
{
    GLuint t, f;
    UBYTE *got = malloc(128 * 128 * 4);
    ULONG t0, t1;
    int l, k, ok = 1;
    char msg[160];
    if (!got)
        return;
    t = texture(512, 512, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "target: 512x512 complete");
    quadrants(0);
    glBindTexture(GL_TEXTURE_2D, t);
    glGenerateMipmap(GL_TEXTURE_2D); /* lays the chain out */
    /* Drawn again, and mipmapped: the case refract has every frame. */
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    quadrants(0.5f);
    glBindTexture(GL_TEXTURE_2D, t);
    glGenerateMipmap(GL_TEXTURE_2D);
    for (l = 2; l <= 8; l++) {
        int s = 512 >> l, q = s / 4; /* a texel in each quadrant, away from the edges */
        static const float want[4][3] = {{1, 0.5f, 0}, {0, 1, 0.5f}, {0.5f, 0, 1}, {1, 1, 0.5f}};
        int at[4][2];
        if (s < 2)
            break;
        at[0][0] = q; at[0][1] = q;
        at[1][0] = s - 1 - q; at[1][1] = q;
        at[2][0] = q; at[2][1] = s - 1 - q;
        at[3][0] = s - 1 - q; at[3][1] = s - 1 - q;
        draw_level(t, l, s, s, got);
        for (k = 0; k < 4; k++) {
            const UBYTE *p = got + (at[k][1] * s + at[k][0]) * 4;
            if (!near(p[0], want[k][0], 3) || !near(p[1], want[k][1], 3) || !near(p[2], want[k][2], 3)) {
                ok = 0;
                printf("  level %d quadrant %d: %02x%02x%02x\n", l, k, p[0], p[1], p[2]);
            }
        }
    }
    check(ok, "target: drawn, mipmapped: each level's four quadrants (levels 2..8)");
    t0 = now_us();
    for (k = 0; k < 20; k++) {
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        quadrants((float)(k & 1));
        glBindTexture(GL_TEXTURE_2D, t);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glFinish();
    t1 = now_us();
    sprintf(msg, "target: 20 frames of a 512x512 draw and glGenerateMipmap, %lu us each", (unsigned long)((t1 - t0) / 20));
    check(glGetError() == GL_NO_ERROR, msg);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &f);
    glDeleteTextures(1, &t);
    free(got);
}

static void test_formats(void)
{
    static UWORD p565[64 * 64], p4444[64 * 64];
    UBYTE got[8 * 8 * 4];
    GLuint t;
    int i;
    char msg[120];
    for (i = 0; i < 64 * 64; i++) {
        p565[i] = (UWORD)((20 << 11) | (40 << 5) | 10); /* 20/31, 40/63, 10/31 */
        p4444[i] = (UWORD)((3 << 12) | (12 << 8) | (6 << 4) | 15);
    }
    /* Each twice: the first glGenerateMipmap lays the chain out, the second
     * (after a new level 0) is the TFU's. */
    t = texture(64, 64, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, p565);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, p565);
    glGenerateMipmap(GL_TEXTURE_2D);
    draw_level(t, 3, 8, 8, got);
    sprintf(msg, "RGB565: level 3 %02x%02x%02x (a5a252, each within 8)", got[0], got[1], got[2]);
    check(near(got[0], 20 / 31.0f, 8) && near(got[1], 40 / 63.0f, 8) && near(got[2], 10 / 31.0f, 8), msg);
    glDeleteTextures(1, &t);
    t = texture(64, 64, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, p4444);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, p4444);
    glGenerateMipmap(GL_TEXTURE_2D);
    draw_level(t, 3, 8, 8, got);
    sprintf(msg, "RGBA4: level 3 %02x%02x%02x (33cc66, each within 17)", got[0], got[1], got[2]);
    check(near(got[0], 3 / 15.0f, 17) && near(got[1], 12 / 15.0f, 17) && near(got[2], 6 / 15.0f, 17), msg);
    glDeleteTextures(1, &t);
}

/* Level 0 of texture t (w x h, within the screen) drawn 1:1 against
 * `want` (RGBA rows, bottom up): the number of texels whose colour
 * differs by more than tol in a channel. */
static int level0_wrong(GLuint t, int w, int h, const UBYTE *want, int tol)
{
    UBYTE *got = malloc((size_t)w * h * 4);
    int i, c, bad = 0;
    if (!got)
        return w * h;
    draw_level(t, 0, w, h, got);
    for (i = 0; i < w * h; i++)
        for (c = 0; c < 3; c++) {
            int d = got[i * 4 + c] - want[i * 4 + c];
            if (d > tol || d < -tol) {
                bad++;
                break;
            }
        }
    free(got);
    return bad;
}

/* Level 0 of a texture larger than the screen (tw x th, `want` its rows):
 * the screen-sized windows at (0,0), at the far corner and in the middle,
 * each drawn 1:1; the number of texels wrong. */
static int big_wrong(GLuint t, int tw, int th, const UBYTE *want)
{
    static const GLfloat pos[] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};
    UBYTE *got = malloc(W * H * 4);
    int win, x, y, c, bad = 0;
    if (!got)
        return W * H;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUseProgram(tex1);
    for (win = 0; win < 3; win++) {
        int x0 = win == 0 ? 0 : win == 1 ? tw - W : (tw - W) / 2;
        int y0 = win == 0 ? 0 : win == 1 ? th - H : (th - H) / 2;
        GLfloat tc[8];
        tc[0] = (GLfloat)x0 / tw;       tc[1] = (GLfloat)y0 / th;
        tc[2] = (GLfloat)(x0 + W) / tw; tc[3] = tc[1];
        tc[4] = tc[0];                  tc[5] = (GLfloat)(y0 + H) / th;
        tc[6] = tc[2];                  tc[7] = tc[5];
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, pos);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, tc);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(1);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, got);
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                for (c = 0; c < 3; c++)
                    if (got[(y * W + x) * 4 + c] != want[((y0 + y) * tw + x0 + x) * 4 + c]) {
                        bad++;
                        break;
                    }
    }
    free(got);
    return bad;
}

/* Uploads the TFU tiles (whole levels of 512x512 texels and more in RGBA8,
 * 128x128 in other formats; probe U keeps them on the CPU): what is drawn
 * is what was given. */
static void test_tfu_upload(void)
{
    UBYTE *img = malloc(600 * 520 * 4), *want = malloc(600 * 520 * 4);
    UWORD *p565 = malloc(256 * 128 * 2);
    GLuint t;
    int i, x, y, bad;
    char msg[160];
    if (!img || !want || !p565) {
        check(0, "memory for the upload test");
        free(img); free(want); free(p565);
        return;
    }
    for (i = 0; i < 512 * 512 * 4; i++)
        img[i] = (UBYTE)noise((ULONG)i + 777);
    t = texture(512, 512, GL_RGBA, GL_UNSIGNED_BYTE, img);
    bad = big_wrong(t, 512, 512, img);
    sprintf(msg, "tfu upload: glTexImage2D 512x512 RGBA8, three windows drawn 1:1: %d texels wrong", bad);
    check(bad == 0, msg);
    /* A box the CPU stores over it (not a whole level): no stale cache
     * line of the TFU's level may spoil it or its neighbours. */
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            UBYTE *p = want + (y * 32 + x) * 4;
            p[0] = 0x12; p[1] = 0x34; p[2] = 0x56; p[3] = 0xFF;
        }
    glBindTexture(GL_TEXTURE_2D, t);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 64, 32, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, want);
    memcpy(want, img, 512 * 512 * 4);
    for (y = 32; y < 64; y++)
        for (x = 64; x < 96; x++) {
            UBYTE *p = want + (y * 512 + x) * 4;
            p[0] = 0x12; p[1] = 0x34; p[2] = 0x56; p[3] = 0xFF;
        }
    bad = big_wrong(t, 512, 512, want);
    sprintf(msg, "tfu upload: then a 32x32 glTexSubImage2D by the CPU: %d texels wrong", bad);
    check(bad == 0, msg);
    /* The whole level again (the TFU's): the box is gone. */
    for (i = 0; i < 512 * 512 * 4; i++)
        img[i] = (UBYTE)noise((ULONG)i + 4242);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 512, GL_RGBA, GL_UNSIGNED_BYTE, img);
    bad = big_wrong(t, 512, 512, img);
    sprintf(msg, "tfu upload: then glTexSubImage2D of the whole level: %d texels wrong", bad);
    check(bad == 0, msg);
    glDeleteTextures(1, &t);

    /* Not a power of two: 600x520 (its own padding). */
    for (i = 0; i < 600 * 520 * 4; i++)
        img[i] = (UBYTE)noise((ULONG)i + 99);
    t = texture(600, 520, GL_RGBA, GL_UNSIGNED_BYTE, img);
    bad = big_wrong(t, 600, 520, img);
    sprintf(msg, "tfu upload: glTexImage2D 600x520 RGBA8: %d texels wrong", bad);
    check(bad == 0, msg);
    glDeleteTextures(1, &t);

    /* RGB565: packed, then tiled; drawn as 8 bits a channel. */
    for (i = 0; i < 256 * 128; i++) {
        ULONG n = noise((ULONG)i + 5);
        int r = (int)(n & 31), g = (int)((n >> 5) & 63), b = (int)((n >> 11) & 31);
        p565[i] = (UWORD)((r << 11) | (g << 5) | b);
        want[i * 4] = (UBYTE)((r * 255 + 15) / 31);
        want[i * 4 + 1] = (UBYTE)((g * 255 + 31) / 63);
        want[i * 4 + 2] = (UBYTE)((b * 255 + 15) / 31);
    }
    t = texture(256, 128, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, p565);
    bad = level0_wrong(t, 256, 128, want, 1);
    sprintf(msg, "tfu upload: glTexImage2D 256x128 RGB565: %d texels wrong", bad);
    check(bad == 0, msg);
    glDeleteTextures(1, &t);
    free(img);
    free(want);
    free(p565);
}

/* How long an upload takes (the plan's question: the TFU for uploads), for
 * glTexImage2D and glTexSubImage2D of the whole image, each after a draw
 * has used the texture (as a program's texture changes do). */
static void test_uploadtime(void)
{
    static const int sizes[2] = {256, 1024};
    UBYTE *img = malloc(1024 * 1024 * 4), got[4];
    int s, i;
    char msg[160];
    if (!img)
        return;
    for (i = 0; i < 1024 * 1024 * 4; i++)
        img[i] = (UBYTE)noise((ULONG)i);
    for (s = 0; s < 2; s++) {
        int n = sizes[s];
        ULONG t0, t1, t2, t3;
        GLuint t = texture(n, n, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        draw_level(t, 0, 1, 1, got);
        t0 = now_us();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
        t1 = now_us();
        draw_level(t, 0, 1, 1, got);
        t2 = now_us();
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, img);
        t3 = now_us();
        sprintf(msg, "upload time %dx%d RGBA8: glTexImage2D %lu us, glTexSubImage2D %lu us", n, n,
                (unsigned long)(t1 - t0), (unsigned long)(t3 - t2));
        check(glGetError() == GL_NO_ERROR, msg);
        glDeleteTextures(1, &t);
        t = texture(n, n, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        draw_level(t, 0, 1, 1, got);
        t0 = now_us();
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n, n, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, img);
        t1 = now_us();
        sprintf(msg, "upload time %dx%d RGB565: glTexSubImage2D %lu us", n, n, (unsigned long)(t1 - t0));
        check(glGetError() == GL_NO_ERROR, msg);
        glDeleteTextures(1, &t);
    }
    free(img);
}

int main(void)
{
    ULONG err = 0, before, after;
    struct BitMap *bm;
    void *ctx;
    char msg[120];
    printf("ogles2 M6: glGenerateMipmap with the TFU\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    if (!GfxBase || !OGLES2Open() || OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK, &treq.tr_node, 0)) {
        printf("needs graphics 39, timer.device and %s\n", OGLES2NAME);
        return 20;
    }
    TimerBase = treq.tr_node.io_Device;
    bm = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    before = AvailMem(MEMF_ANY);
    if (!bm || !(ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, TAG_DONE))) {
        printf("FAIL: create a bitmap context (error %lu)\n", (unsigned long)err);
        return 20;
    }
    aglMakeCurrent(ctx);
    smooth = link(smooth_vs, smooth_fs, "a_position", "a_color");
    tex1 = link(tex1_vs, tex1_fs, "a_position", "a_texcoord");
    check(smooth && tex1, "smooth and tex1: link");
    glUseProgram(smooth);
    glUniformMatrix4fv(glGetUniformLocation(smooth, "u_mvp"), 1, GL_FALSE, identity);
    glUseProgram(tex1);
    glUniform1i(glGetUniformLocation(tex1, "u_tex"), 0);

    test_upload();
    test_npot();
    test_target();
    test_formats();
    test_tfu_upload();
    test_uploadtime();

    glUseProgram(0);
    glDeleteProgram(smooth);
    glDeleteProgram(tex1);
    check(glGetError() == GL_NO_ERROR, "no GL error left over");
    aglDestroyContext(ctx);
    after = AvailMem(MEMF_ANY);
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)before);
    check(after + 32768 >= before, msg);
    FreeBitMap(bm);
    CloseDevice(&treq.tr_node);
    OGLES2Close();
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
