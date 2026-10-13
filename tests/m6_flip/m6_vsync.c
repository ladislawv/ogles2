/*
 * M6 presentation with GL (plan 7.2): ogles2's fullscreen 640x480 context,
 * the white bar of m6_flip drawn with GL (48 pixels wide, 480 pixels a
 * second, its place from the time), 6 seconds each way, after a second with
 * only the way's number (that many white squares at the top):
 *   5 vsync off: the screen's own bitmap (single buffer, as fast as it goes)
 *   6 vsync switched on (aglSetParams2): two screen buffers, ChangeScreenBuffer
 *   7 vsync switched off again: the single buffer back
 *   8 a new context with OGLES2_CCT_VSYNC TRUE from the start
 * Watch for a torn bar; the frame rates are printed.
 *   usage: m6_vsync
 */
#include <stdio.h>
#include <exec/types.h>
#include <devices/timer.h>
#include <graphics/displayinfo.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/timer.h>
#include <proto/ogles2.h>
#include "../common/smooth_shaders.h"

struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
struct Device *TimerBase;
static struct timerequest treq;
static GLuint smooth;

#define W 640
#define H 480
#define BAR 48
#define TOP 80
#define SPEED 480
#define SECONDS 6

static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static ULONG now_us(void)
{
    struct EClockVal ev;
    ULONG f = ReadEClock(&ev);
    return (ULONG)(((unsigned long long)ev.ev_hi << 32 | ev.ev_lo) * 1000000ULL / f);
}

static GLuint link(const char *vs, const char *fs)
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
    glBindAttribLocation(p, 0, "a_position");
    glBindAttribLocation(p, 1, "a_color");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

/* A white rectangle in screen pixels (y down from the top). */
static void rect(int x, int y, int w, int h)
{
    GLfloat v[12];
    GLfloat x0 = (GLfloat)x / W * 2 - 1, x1 = (GLfloat)(x + w) / W * 2 - 1;
    GLfloat y0 = 1 - (GLfloat)(y + h) / H * 2, y1 = 1 - (GLfloat)y / H * 2;
    v[0] = x0; v[1] = y0; v[2] = 0;
    v[3] = x1; v[4] = y0; v[5] = 0;
    v[6] = x0; v[7] = y1; v[8] = 0;
    v[9] = x1; v[10] = y1; v[11] = 0;
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glVertexAttrib4f(1, 1, 1, 1, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* One frame: the background, the way's number, the bar at x (x < 0: none). */
static void frame(int way, int x)
{
    int i;
    glClearColor(0x20 / 255.0f, 0x30 / 255.0f, 0x40 / 255.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(smooth);
    for (i = 0; i < way; i++)
        rect(24 + i * 72, 16, 48, 48);
    if (x >= 0)
        rect(x, TOP, BAR, H - TOP);
    aglSwapBuffers();
}

static void way_run(int way, const char *name)
{
    ULONG t0, t, frames = 0;
    t0 = now_us();
    while ((t = now_us() - t0) < 1000000UL) /* the number alone */
        frame(way, -1);
    t0 = now_us();
    while ((t = now_us() - t0) < SECONDS * 1000000UL) {
        frame(way, (int)((unsigned long long)t * SPEED / 1000000UL % (W - BAR)));
        frames++;
    }
    printf("%s: %lu frames in %d s, %lu fps\n", name, (unsigned long)frames, SECONDS,
           (unsigned long)(frames / SECONDS));
}

/* A 640x480 mode of the pixel formats ogles2 renders (BGRA32, RGBA32), as
 * ports/ioq3-gl2's glimp finds one. */
static ULONG find_mode(void)
{
    ULONG id = (ULONG)INVALID_ID;
    while ((id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
        ULONG f;
        if (!IsCyberModeID(id))
            continue;
        f = GetCyberIDAttr(CYBRIDATTR_PIXFMT, id);
        if ((f == PIXFMT_BGRA32 || f == PIXFMT_RGBA32) && GetCyberIDAttr(CYBRIDATTR_WIDTH, id) == W &&
            GetCyberIDAttr(CYBRIDATTR_HEIGHT, id) == H)
            return id;
    }
    return (ULONG)INVALID_ID;
}

static void *open_ctx(ULONG mode, BOOL vsync)
{
    ULONG err = 0;
    void *ctx = aglCreateContextTags2(&err, OGLES2_CCT_MODEID, mode, OGLES2_CCT_VSYNC, (ULONG)vsync, TAG_DONE);
    if (!ctx) {
        printf("FAIL: fullscreen context (vsync %d): error %lu\n", (int)vsync, (unsigned long)err);
        return 0;
    }
    aglMakeCurrent(ctx);
    smooth = link(smooth_vs, smooth_fs);
    glUseProgram(smooth);
    glUniformMatrix4fv(glGetUniformLocation(smooth, "u_mvp"), 1, GL_FALSE, identity);
    glViewport(0, 0, W, H);
    return ctx;
}

int main(void)
{
    ULONG mode;
    void *ctx;
    printf("ogles2 M6: presentation with GL (watch the white bar)\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!GfxBase || !CyberGfxBase || !OGLES2Open() ||
        OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK, &treq.tr_node, 0)) {
        printf("needs graphics 39, cybergraphics 41, timer.device and %s\n", OGLES2NAME);
        return 20;
    }
    TimerBase = treq.tr_node.io_Device;
    mode = find_mode();
    if (mode == (ULONG)INVALID_ID) {
        printf("FAIL: no 640x480 BGRA32 or RGBA32 mode\n");
        return 20;
    }
    printf("screen mode 0x%08lx\n", (unsigned long)mode);
    if ((ctx = open_ctx(mode, FALSE)) != 0) {
        way_run(5, "5 ogles2, vsync off");
        aglSetParamsTags2(OGLES2_CCT_VSYNC, TRUE, TAG_DONE);
        way_run(6, "6 ogles2, vsync switched on");
        aglSetParamsTags2(OGLES2_CCT_VSYNC, FALSE, TAG_DONE);
        way_run(7, "7 ogles2, vsync switched off again");
        glDeleteProgram(smooth);
        aglDestroyContext(ctx);
    }
    if ((ctx = open_ctx(mode, TRUE)) != 0) {
        way_run(8, "8 ogles2, a context with vsync from the start");
        glDeleteProgram(smooth);
        aglDestroyContext(ctx);
    }
    CloseDevice(&treq.tr_node);
    OGLES2Close();
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    printf("done\n");
    return 0;
}
