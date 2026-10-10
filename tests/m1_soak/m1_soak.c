/*
 * M1 soak test (plan section 9, M1 exit criteria): 10,000 frames in a
 * window ogles2 opens, with no leak and no GPU timeout.
 *   usage: m1_soak [FRAMES]     (default 10000)
 *
 * Every frame: a tile-buffer clear, a spinning smooth triangle and a ring of
 * 50 small ones (client arrays); every 7th frame a scissored clear after the
 * draws (a rectangle), every 11th the stencil test, every 13th depth.
 * Every 1000th frame the centre and a corner are read back. The library's
 * log has the dropped frames and the GPU memory still in use at the end;
 * this program checks free memory and GL errors. About 30 s for 10,000.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/timer.h>
#include <proto/ogles2.h>
#include "../common/smooth_shaders.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
struct Device *TimerBase;
static struct timerequest timer_req;


static double now_ms(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return tv.tv_secs * 1000.0 + tv.tv_micro / 1000.0;
}

int main(int argc, char **argv)
{
    ULONG err = 0, w = 0, h = 0, before, after;
    long frames = argc > 1 ? atol(argv[1]) : 10000, f;
    int errors = 0, bad_pixels = 0, k;
    struct Window *win = 0;
    void *ctx;
    GLuint vs, fs, prog;
    GLint loc_pos, loc_col, loc_mvp;
    static GLfloat ring_pos[50 * 9], ring_col[50 * 12];
    static const GLfloat tri_col[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    static const GLfloat square[12] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};
    double t0, t1;

    printf("ogles2 M1 soak: %ld frames\n", frames);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase ||
        OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&timer_req, 0) ||
        !OGLES2Open()) {
        printf("needs intuition, graphics, cybergraphics, timer.device and %s\n", OGLES2NAME);
        return 20;
    }
    TimerBase = timer_req.tr_node.io_Device;
    for (k = 0; k < 50; k++) {
        float a = k * 6.2831853f / 50, x = 0.85f * cosf(a), y = 0.85f * sinf(a);
        GLfloat *p = &ring_pos[k * 9];
        p[0] = x - 0.04f; p[1] = y - 0.04f; p[2] = 0.5f;
        p[3] = x + 0.04f; p[4] = y - 0.04f; p[5] = 0.5f;
        p[6] = x;         p[7] = y + 0.05f; p[8] = 0.5f;
        memcpy(&ring_col[k * 12], tri_col, sizeof(tri_col));
    }

    before = AvailMem(MEMF_ANY);
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_DEPTH, 24, OGLES2_CCT_STENCIL, 8,
                                OGLES2_CCT_PISTORM_WIDTH, 320, OGLES2_CCT_PISTORM_HEIGHT, 240,
                                OGLES2_CCT_GET_WIDTH, (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h,
                                OGLES2_CCT_PISTORM_GET_WINDOW, (ULONG)&win, TAG_DONE);
    if (!ctx || !win) {
        printf("FAIL: create a context with its own window (error %lu)\n", (unsigned long)err);
        return 20;
    }
    aglMakeCurrent(ctx);
    vs = glCreateShader(GL_VERTEX_SHADER);
    fs = glCreateShader(GL_FRAGMENT_SHADER);
    {
        const char *s1 = smooth_vs, *s2 = smooth_fs;
        glShaderSource(vs, 1, &s1, 0);
        glShaderSource(fs, 1, &s2, 0);
    }
    glCompileShader(vs);
    glCompileShader(fs);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glUseProgram(prog);
    loc_pos = glGetAttribLocation(prog, "a_position");
    loc_col = glGetAttribLocation(prog, "a_color");
    loc_mvp = glGetUniformLocation(prog, "u_mvp");

    t0 = now_ms();
    for (f = 0; f < frames; f++) {
        float a = f * 0.02f, c = cosf(a), s = sinf(a);
        GLfloat m[16] = {0};
        GLfloat tri[9];
        m[0] = c; m[1] = s; m[4] = -s; m[5] = c; m[10] = 1; m[15] = 1;
        tri[0] = -0.6f; tri[1] = -0.5f; tri[2] = 0;
        tri[3] = 0.6f;  tri[4] = -0.5f; tri[5] = 0;
        tri[6] = 0.0f;  tri[7] = 0.6f;  tri[8] = 0;

        glClearColor(0.1f, 0.1f, 0.2f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        if (f % 13 == 0)
            glEnable(GL_DEPTH_TEST);
        glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, m);
        glVertexAttribPointer(loc_pos, 3, GL_FLOAT, GL_FALSE, 0, tri);
        glEnableVertexAttribArray(loc_pos);
        glVertexAttribPointer(loc_col, 4, GL_FLOAT, GL_FALSE, 0, tri_col);
        glEnableVertexAttribArray(loc_col);
        if (f % 11 == 0) { /* mark the triangle in the stencil buffer */
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, 1, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
        m[0] = 1; m[1] = 0; m[4] = 0; m[5] = 1;
        glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, m);
        glVertexAttribPointer(loc_pos, 3, GL_FLOAT, GL_FALSE, 0, ring_pos);
        glVertexAttribPointer(loc_col, 4, GL_FLOAT, GL_FALSE, 0, ring_col);
        glDrawArrays(GL_TRIANGLES, 0, 150);
        if (f % 11 == 0) { /* a grey veil outside the triangle */
            glStencilFunc(GL_NOTEQUAL, 1, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glDisableVertexAttribArray(loc_col);
            glVertexAttrib4f(loc_col, 0.3f, 0.3f, 0.3f, 1);
            glVertexAttribPointer(loc_pos, 3, GL_FLOAT, GL_FALSE, 0, square);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisable(GL_STENCIL_TEST);
        }
        if (f % 7 == 0) { /* a scissored clear after the draws: a rectangle */
            glEnable(GL_SCISSOR_TEST);
            glScissor((GLint)(f % 200), 10, 40, 30);
            glClearColor(1, 1, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        }
        glDisable(GL_DEPTH_TEST);
        if (f % 1000 == 999) {
            /* The centre is inside the spinning triangle; a top corner is
             * the clear colour (or the veil on stencil frames). */
            aglSwapBuffers();
            {
                ULONG centre = ReadRGBPixel(win->RPort, (UWORD)(win->BorderLeft + w / 2),
                                            (UWORD)(win->BorderTop + h / 2)) & 0xFFFFFF;
                ULONG corner = ReadRGBPixel(win->RPort, (UWORD)(win->BorderLeft + 2),
                                            (UWORD)(win->BorderTop + 2)) & 0xFFFFFF;
                int ok = centre != 0x1a1a33 && centre != 0 && (corner == 0x1a1a33 || corner == 0x4c4c4c || corner == 0x4d4d4d);
                if (!ok)
                    bad_pixels++;
                printf("     frame %ld: centre %06lx, corner %06lx%s\n", f + 1, (unsigned long)centre,
                       (unsigned long)corner, ok ? "" : "  <- unexpected");
                fflush(stdout);
            }
        } else {
            aglSwapBuffers();
        }
        if (glGetError() != GL_NO_ERROR)
            errors++;
    }
    t1 = now_ms();
    glDeleteProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    aglDestroyContext(ctx);
    after = AvailMem(MEMF_ANY);

    printf("     %ld frames in %ld ms (%ld.%ld ms per frame)\n", frames, (long)(t1 - t0),
           (long)((t1 - t0) / frames), (long)((t1 - t0) * 10 / frames) % 10);
    printf("%s: no GL error (%d frames with one)\n", errors ? "FAIL" : "PASS", errors);
    printf("%s: read-back pixels as expected (%d wrong)\n", bad_pixels ? "FAIL" : "PASS", bad_pixels);
    printf("%s: destroy returns the memory (%ld bytes difference)\n",
           after + 16384 >= before ? "PASS" : "FAIL", (long)after - (long)before);
    printf("     the log has dropped frames and GPU memory at the end\n");
    OGLES2Close();
    CloseDevice((struct IORequest *)&timer_req);
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return errors || bad_pixels || after + 16384 < before ? 10 : 0;
}
