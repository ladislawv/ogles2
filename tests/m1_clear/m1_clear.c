/*
 * M1 step 1 test: clear and present on the GPU, for every surface kind
 * (plan section 7.2), checked by reading back what is displayed.
 *   usage: m1_clear [WINDOW] [ADOPT] [BITMAP] [FULLSCREEN] [SCREENWINDOW]
 *          (default: all)
 *
 * For each kind: six clear colours, each held 1 s and read back at the centre
 * and the four corners of what is actually shown (the window after the
 * ClipBlit, the screen, or the application's bitmap; SCREENWINDOW is a
 * window that is all of a screen of the test's, as SDL's fullscreen); a frame that clears
 * only depth must keep the picture (the tile buffer loads it); a 2 s fade to
 * watch; 100 frames timed; and the context's GPU memory must be returned when
 * it is destroyed. About 10 s per kind.
 */
#include <stdio.h>
#include <string.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <cybergraphx/cybergraphics.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/timer.h>
#include <proto/ogles2.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
struct Device *TimerBase;
static struct timerequest timer_req;
static int failures, checks;

static void check(int ok, const char *fmt, const char *kind)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s: %s\n", ok ? "PASS" : "FAIL", kind, fmt);
}

static double now_ms(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return tv.tv_secs * 1000.0 + tv.tv_micro / 1000.0;
}

/* Where the test reads back what is shown. */
struct readback {
    struct RastPort *rp; /* window or screen rastport, or one over the bitmap */
    int x0, y0;          /* inner origin (window borders) */
    int w, h;
    struct RastPort own;
};

static ULONG expect_rgb(float r, float g, float b)
{
    return ((ULONG)(r * 255.0f + 0.5f) << 16) | ((ULONG)(g * 255.0f + 0.5f) << 8) |
           (ULONG)(b * 255.0f + 0.5f);
}

static int near(ULONG a, ULONG b)
{
    int i;
    for (i = 0; i < 24; i += 8) {
        int d = (int)((a >> i) & 255) - (int)((b >> i) & 255);
        if (d < -1 || d > 1)
            return 0;
    }
    return 1;
}

/* Centre and four corners must all show colour want. */
static int shows(struct readback *r, ULONG want, ULONG *got)
{
    int px[5], py[5], i;
    px[0] = r->w / 2;  py[0] = r->h / 2;
    px[1] = 0;         py[1] = 0;
    px[2] = r->w - 1;  py[2] = 0;
    px[3] = 0;         py[3] = r->h - 1;
    px[4] = r->w - 1;  py[4] = r->h - 1;
    for (i = 0; i < 5; i++) {
        *got = ReadRGBPixel(r->rp, (UWORD)(r->x0 + px[i]), (UWORD)(r->y0 + py[i])) & 0xFFFFFF;
        if (!near(*got, want))
            return 0;
    }
    return 1;
}

static void run_kind(const char *kind, void *ctx, struct readback *r)
{
    static const float colours[6][3] = {
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}, {0.25f, 0.5f, 0.75f}};
    char msg[160];
    ULONG got = 0;
    int i, ok;
    double t0, t1;

    aglMakeCurrent(ctx);
    /* Each colour stays 1 s, so a person can follow (black, red, green, blue,
     * white, light blue). */
    for (i = 0; i < 6; i++) {
        ULONG want = expect_rgb(colours[i][0], colours[i][1], colours[i][2]);
        glClearColor(colours[i][0], colours[i][1], colours[i][2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        aglSwapBuffers();
        Delay(50);
        ok = shows(r, want, &got);
        sprintf(msg, "clear to %06lx shows %06lx", (unsigned long)want, (unsigned long)got);
        check(ok, msg, kind);
    }
    /* A frame that clears only depth: the GPU job must load the colour that
     * is shown and store it back unchanged (magenta stays, 1 s). */
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    aglSwapBuffers();
    glClearColor(0, 1, 0, 1); /* not applied: colour is not cleared below */
    glClear(GL_DEPTH_BUFFER_BIT);
    aglSwapBuffers();
    Delay(50);
    ok = shows(r, 0xFF00FF, &got);
    sprintf(msg, "a frame that clears only depth keeps the picture (%06lx)", (unsigned long)got);
    check(ok, msg, kind);
    check(glGetError() == GL_NO_ERROR, "no GL error", kind);

    /* A slow fade from red to blue, 2 s, to watch for flicker. */
    for (i = 0; i <= 50; i++) {
        float f = (float)i / 50.0f;
        glClearColor(1.0f - f, 0.0f, f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        aglSwapBuffers();
        Delay(2);
    }

    t0 = now_ms();
    for (i = 0; i < 100; i++) {
        float f = (float)i / 100.0f;
        glClearColor(f, 1.0f - f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        aglSwapBuffers();
    }
    t1 = now_ms();
    printf("     %s: 100 clear+swap frames in %ld ms (%ld.%ld ms per frame)\n", kind,
           (long)(t1 - t0), (long)((t1 - t0) / 100), (long)((t1 - t0) / 10) % 10);
}

static void finish_kind(const char *kind, void *ctx, ULONG mem_before)
{
    char msg[120];
    ULONG after;
    aglDestroyContext(ctx);
    after = AvailMem(MEMF_ANY);
    /* the context's ~17 MB of pools come back; 16 KB of slack for other tasks */
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)mem_before);
    check(after + 16384 >= mem_before, msg, kind);
}

static void test_own_window(void)
{
    ULONG err = 0, w = 0, h = 0, before = AvailMem(MEMF_ANY);
    struct readback r;
    struct Window *win = 0;
    void *ctx = aglCreateContextTags2(&err, OGLES2_CCT_DEPTH, 16, OGLES2_CCT_PISTORM_WIDTH, 320,
                                      OGLES2_CCT_PISTORM_HEIGHT, 240, OGLES2_CCT_GET_WIDTH,
                                      (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h,
                                      OGLES2_CCT_PISTORM_GET_WINDOW, (ULONG)&win, TAG_DONE);
    check(ctx != 0, "create (own window)", "own window");
    if (!ctx) {
        printf("     error %lu\n", (unsigned long)err);
        return;
    }
    check(w == 320 && h == 240 && win != 0, "GET_WIDTH/GET_HEIGHT 320x240, GET_WINDOW",
          "own window");
    if (!win) {
        aglDestroyContext(ctx);
        return;
    }
    {
        r.rp = win->RPort;
        r.x0 = win->BorderLeft;
        r.y0 = win->BorderTop;
        r.w = (int)w;
        r.h = (int)h;
    }
    run_kind("own window", ctx, &r);
    finish_kind("own window", ctx, before);
}

static void test_adopted_window(void)
{
    ULONG err = 0, before;
    struct readback r;
    void *ctx;
    struct Window *win = OpenWindowTags(0, WA_InnerWidth, 300, WA_InnerHeight, 200, WA_Left, 60,
                                        WA_Top, 60, WA_Title, (ULONG) "m1_clear: application window",
                                        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_Activate, TRUE,
                                        TAG_DONE);
    if (!win) {
        check(0, "open the application's window", "application window");
        return;
    }
    before = AvailMem(MEMF_ANY); /* after our own window: only the context is measured */
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_WINDOW, (ULONG)win, OGLES2_CCT_DEPTH, 24,
                                TAG_DONE);
    check(ctx != 0, "create (WINDOW tag)", "application window");
    if (ctx) {
        r.rp = win->RPort;
        r.x0 = win->BorderLeft;
        r.y0 = win->BorderTop;
        r.w = win->Width - win->BorderLeft - win->BorderRight;
        r.h = win->Height - win->BorderTop - win->BorderBottom;
        run_kind("application window", ctx, &r);
        finish_kind("application window", ctx, before);
    } else {
        printf("     error %lu\n", (unsigned long)err);
    }
    CloseWindow(win); /* ours: the library must not have closed it */
}

static void test_bitmap(void)
{
    ULONG err = 0, before;
    struct readback r;
    void *ctx;
    struct BitMap *bm = AllocBitMap(256, 128, 32,
                                    BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    if (!bm) {
        check(0, "allocate a 256x128 BGRA32 bitmap", "application bitmap");
        return;
    }
    before = AvailMem(MEMF_ANY); /* after our own bitmap: only the context is measured */
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, TAG_DONE);
    check(ctx != 0, "create (BITMAP tag)", "application bitmap");
    if (ctx) {
        InitRastPort(&r.own);
        r.own.BitMap = bm;
        r.rp = &r.own;
        r.x0 = r.y0 = 0;
        r.w = 256;
        r.h = 128;
        run_kind("application bitmap", ctx, &r);
        finish_kind("application bitmap", ctx, before);
    } else {
        printf("     error %lu\n", (unsigned long)err);
    }
    FreeBitMap(bm);
}

static ULONG find_mode(void)
{
    ULONG id = NextDisplayInfo(INVALID_ID), best = INVALID_ID;
    while (id != (ULONG)INVALID_ID) {
        if (IsCyberModeID(id)) {
            ULONG f = GetCyberIDAttr(CYBRIDATTR_PIXFMT, id);
            ULONG w = GetCyberIDAttr(CYBRIDATTR_WIDTH, id);
            ULONG h = GetCyberIDAttr(CYBRIDATTR_HEIGHT, id);
            if ((f == PIXFMT_BGRA32 || f == PIXFMT_RGBA32) && w >= 320 && w <= 800 && h >= 240) {
                if (w == 640 && h == 480)
                    return id;
                if (best == (ULONG)INVALID_ID)
                    best = id;
            }
        }
        id = NextDisplayInfo(id);
    }
    return best;
}

static void test_fullscreen(void)
{
    ULONG err = 0, w = 0, h = 0, before = AvailMem(MEMF_ANY);
    struct readback r;
    void *ctx;
    ULONG mode = find_mode();
    struct Window *win;
    if (mode == (ULONG)INVALID_ID) {
        printf("SKIP: fullscreen: no 32-bit BGRA/RGBA RTG mode of 320x240 to 800x600\n");
        return;
    }
    printf("     fullscreen: mode 0x%08lx\n", (unsigned long)mode);
    win = 0;
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_MODEID, mode, OGLES2_CCT_DEPTH, 32,
                                OGLES2_CCT_GET_WIDTH, (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h,
                                OGLES2_CCT_PISTORM_GET_WINDOW, (ULONG)&win, TAG_DONE);
    check(ctx != 0, "create (MODEID tag)", "fullscreen");
    if (!ctx) {
        printf("     error %lu\n", (unsigned long)err);
        return;
    }
    if (!win) {
        check(0, "GET_WINDOW returns the backdrop window", "fullscreen");
        aglDestroyContext(ctx);
        return;
    }
    {
        struct Screen *scr = win->WScreen; /* ogles2's screen */
        r.rp = &scr->RastPort;
        r.x0 = r.y0 = 0;
        r.w = (int)w;
        r.h = (int)h;
    }
    run_kind("fullscreen", ctx, &r);
    finish_kind("fullscreen", ctx, before);
}

/* SDL's fullscreen: a borderless window at 0,0 that is all of a private
 * 32-bit screen. ogles2 renders straight into the screen (no copy; the log
 * says "all of its screen"). Then the context moves to a smaller window on
 * that screen (the copy again) and back, and glReadPixels reads the screen. */
static void test_screen_window(void)
{
    static const ULONG depths[2] = {24, 32};
    ULONG err = 0, before, w = 0, h = 0, mode = find_mode();
    struct readback r;
    struct Screen *scr = 0;
    struct Window *win, *small;
    void *ctx;
    char msg[120];
    ULONG got = 0;
    UBYTE px[4];
    int i;

    if (mode == (ULONG)INVALID_ID) {
        printf("SKIP: screen window: no 32-bit BGRA/RGBA RTG mode of 320x240 to 800x600\n");
        return;
    }
    for (i = 0; i < 2 && !scr; i++)
        scr = OpenScreenTags(0, SA_DisplayID, mode, SA_Depth, depths[i], SA_Type, CUSTOMSCREEN,
                             SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Exclusive, TRUE, SA_Draggable,
                             FALSE, TAG_DONE);
    if (!scr) {
        check(0, "open a screen of mode", "screen window");
        return;
    }
    win = OpenWindowTags(0, WA_CustomScreen, (ULONG)scr, WA_Left, 0, WA_Top, 0, WA_Width,
                         (ULONG)scr->Width, WA_Height, (ULONG)scr->Height, WA_Flags,
                         WFLG_BORDERLESS | WFLG_ACTIVATE | WFLG_RMBTRAP | WFLG_REPORTMOUSE, TAG_DONE);
    if (!win) {
        check(0, "open a borderless window of the screen's size", "screen window");
        CloseScreen(scr);
        return;
    }
    printf("     screen window: mode 0x%08lx, %ldx%ld\n", (unsigned long)mode, (long)scr->Width,
           (long)scr->Height);
    before = AvailMem(MEMF_ANY);
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_WINDOW, (ULONG)win, OGLES2_CCT_DEPTH, 24,
                                OGLES2_CCT_GET_WIDTH, (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h,
                                TAG_DONE);
    check(ctx != 0, "create (WINDOW tag)", "screen window");
    if (!ctx) {
        printf("     error %lu\n", (unsigned long)err);
        CloseWindow(win);
        CloseScreen(scr);
        return;
    }
    sprintf(msg, "GET_WIDTH/GET_HEIGHT the screen's %ldx%ld (%lux%lu)", (long)scr->Width,
            (long)scr->Height, (unsigned long)w, (unsigned long)h);
    check(w == (ULONG)scr->Width && h == (ULONG)scr->Height, msg, "screen window");
    r.rp = &scr->RastPort; /* what the screen shows */
    r.x0 = r.y0 = 0;
    r.w = (int)w;
    r.h = (int)h;
    run_kind("screen window", ctx, &r);

    /* glReadPixels from the screen: a clear not yet swapped. */
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels((GLint)w / 2, (GLint)h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    got = ((ULONG)px[0] << 16) | ((ULONG)px[1] << 8) | px[2];
    sprintf(msg, "glReadPixels reads the clear (%06lx)", (unsigned long)got);
    check(near(got, expect_rgb(0.25f, 0.5f, 0.75f)) && glGetError() == GL_NO_ERROR, msg,
          "screen window");
    aglSwapBuffers();

    /* A smaller window on the same screen: the copy path, then back. */
    small = OpenWindowTags(0, WA_CustomScreen, (ULONG)scr, WA_Left, 40, WA_Top, 40, WA_Width, 320,
                           WA_Height, 200, WA_Flags, WFLG_BORDERLESS, TAG_DONE);
    if (!small) {
        check(0, "open a 320x200 window on the screen", "screen window");
    } else {
        struct readback rs;
        w = h = 0;
        aglSetParamsTags2(OGLES2_CCT_WINDOW, (ULONG)small, OGLES2_CCT_GET_WIDTH, (ULONG)&w,
                          OGLES2_CCT_GET_HEIGHT, (ULONG)&h, TAG_DONE);
        sprintf(msg, "aglSetParams2 to a 320x200 window on it (%lux%lu)", (unsigned long)w,
                (unsigned long)h);
        check(w == 320 && h == 200, msg, "screen window");
        glViewport(0, 0, 320, 200);
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        aglSwapBuffers();
        Delay(50);
        rs.rp = small->RPort;
        rs.x0 = rs.y0 = 0;
        rs.w = 320;
        rs.h = 200;
        sprintf(msg, "the small window shows the clear to ff0000");
        check(shows(&rs, 0xFF0000, &got), msg, "screen window");
        aglSetParamsTags2(OGLES2_CCT_WINDOW, (ULONG)win, OGLES2_CCT_GET_WIDTH, (ULONG)&w,
                          OGLES2_CCT_GET_HEIGHT, (ULONG)&h, TAG_DONE);
        CloseWindow(small);
        sprintf(msg, "back to the whole-screen window (%lux%lu)", (unsigned long)w, (unsigned long)h);
        check(w == (ULONG)scr->Width && h == (ULONG)scr->Height, msg, "screen window");
        glViewport(0, 0, (GLsizei)w, (GLsizei)h);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        aglSwapBuffers();
        Delay(50);
        sprintf(msg, "the screen shows the clear to 00ff00 again");
        check(shows(&r, 0x00FF00, &got), msg, "screen window");
        check(glGetError() == GL_NO_ERROR, "no GL error", "screen window");
    }
    finish_kind("screen window", ctx, before);
    CloseWindow(win); /* ours: the library must not have closed it */
    if (!CloseScreen(scr))
        check(0, "close the screen (a window left on it?)", "screen window");
}

int main(int argc, char **argv)
{
    int i, all = argc < 2;

    printf("ogles2 M1 step 1: clear and present\n");
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase || OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&timer_req, 0)) {
        printf("needs cybergraphics.library 41 and timer.device\n");
        return 20;
    }
    TimerBase = timer_req.tr_node.io_Device;
    if (!OGLES2Open()) {
        printf("cannot open %s %d\n", OGLES2NAME, OGLES2_VERSION);
        return 20;
    }
    printf("     GL_RENDERER (first context decides): see the log\n");
    for (i = 1; i < argc || all; i++) {
        const char *a = all ? 0 : argv[i];
        if (all || !strcmp(a, "WINDOW"))
            test_own_window();
        if (all || !strcmp(a, "ADOPT"))
            test_adopted_window();
        if (all || !strcmp(a, "BITMAP"))
            test_bitmap();
        if (all || !strcmp(a, "FULLSCREEN"))
            test_fullscreen();
        if (all || !strcmp(a, "SCREENWINDOW"))
            test_screen_window();
        if (all)
            break;
    }
    OGLES2Close();
    CloseDevice((struct IORequest *)&timer_req);
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures,
           checks);
    return failures ? 10 : 0;
}
