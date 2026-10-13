/*
 * M6 presentation investigation (plan 7.2): how a 640x480 32-bit RTG screen
 * on the PiStorm presents, without GL. A white bar 48 pixels wide crosses
 * the screen at 480 pixels a second (its place from the time, so the same
 * motion at any frame rate); each way runs 6 seconds, after a second with
 * only its number on the screen (that many white squares at the top):
 *   1 single buffer, as fast as it goes (the library's fullscreen today)
 *   2 single buffer, WaitTOF before each frame
 *   3 two screen buffers, ChangeScreenBuffer, waiting for the safe message
 *     before drawing into the hidden one
 *   4 the same, waiting for the displayed message too (one flip a refresh)
 * Watch for a broken (torn) bar and for jerky motion; the frame rates are
 * printed. The bar is drawn with FillPixelArray (cybergraphics), as a frame
 * the GPU stores lands in the same bitmap.
 *   usage: m6_flip [WAYS [SECONDS [PRIORITY]]]   e.g. m6_flip 34 10: ways 3 and 4, 10 s each
 */
#include <stdio.h>
#include <stdlib.h>
#include <exec/types.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/timer.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
struct Device *TimerBase;
static struct timerequest treq;

#define W 640
#define H 480
#define BAR 48
#define TOP 80          /* the band with the way's number; the bar is below it */
#define SPEED 480       /* pixels a second, whatever the frame rate (8 a frame at 60 Hz) */
static int seconds = 6; /* each way's run (argument 2) */
#define BACKGROUND 0x00203040

static ULONG now_us(void)
{
    struct EClockVal ev;
    ULONG f = ReadEClock(&ev);
    return (ULONG)(((unsigned long long)ev.ev_hi << 32 | ev.ev_lo) * 1000000ULL / f);
}

/* The screen cleared, with the way's number as that many white squares in
 * the top band. */
static void page(struct RastPort *rp, int way)
{
    int i;
    FillPixelArray(rp, 0, 0, W, H, BACKGROUND);
    for (i = 0; i < way; i++)
        FillPixelArray(rp, (UWORD)(24 + i * 72), 16, 48, 48, 0x00FFFFFF);
}

/* The bar at x in the bitmap of rp; of its place before (old) only what it
 * left is cleared, so a bar standing still never flickers. */
static void bar(struct RastPort *rp, int old, int x)
{
    if (old == x)
        return;
    if (old >= 0) {
        if (x > old && x < old + BAR)
            FillPixelArray(rp, (UWORD)old, TOP, (UWORD)(x - old), H - TOP, BACKGROUND);
        else
            FillPixelArray(rp, (UWORD)old, TOP, BAR, H - TOP, BACKGROUND);
    }
    FillPixelArray(rp, (UWORD)x, TOP, BAR, H - TOP, 0x00FFFFFF);
}

/* Where the bar is `us` microseconds into a way: the same path at any rate. */
static int bar_x(ULONG us)
{
    return (int)((unsigned long long)us * SPEED / 1000000UL % (W - BAR));
}

/* The way's number alone for a second, before it starts. */
static void announce(struct Screen *s, int way)
{
    page(&s->RastPort, way);
    Delay(50);
}

/* Ways 1 and 2: straight into the displayed bitmap. */
static void single(struct Screen *s, int vsync, int way, const char *name)
{
    struct RastPort *rp = &s->RastPort;
    ULONG t0, t, frames = 0;
    int old = -1, x;
    announce(s, way);
    t0 = now_us();
    while ((t = now_us() - t0) < (ULONG)seconds * 1000000UL) {
        if (vsync)
            WaitTOF();
        x = bar_x(t);
        bar(rp, old, x);
        old = x;
        frames++;
    }
    printf("%s: %lu frames in %d s, %lu fps\n", name, (unsigned long)frames, seconds,
           (unsigned long)(frames / seconds));
}

/* Ways 3 and 4: two buffers flipped by ChangeScreenBuffer (RKRM's
 * protocol: draw into a buffer only after its safe message). */
/* The intervals between successive messages of one kind: how many fell in
 * each millisecond, the smallest, the largest, and how many were longer than
 * 1.5 times the most common (a refresh missed). */
#define MAX_IV 2048
static ULONG iv_safe[MAX_IV], iv_disp[MAX_IV];

static void report(const char *what, const ULONG *iv, int n)
{
    int hist[41], i, mode_ms = 0, longer = 0;
    ULONG lo = ~0UL, hi = 0, sum = 0;
    if (n < 2) {
        printf("  %s: %d intervals\n", what, n);
        return;
    }
    for (i = 0; i <= 40; i++)
        hist[i] = 0;
    for (i = 0; i < n; i++) {
        int ms = (int)((iv[i] + 500) / 1000);
        hist[ms > 40 ? 40 : ms]++;
        if (iv[i] < lo) lo = iv[i];
        if (iv[i] > hi) hi = iv[i];
        sum += iv[i];
    }
    for (i = 1; i <= 40; i++)
        if (hist[i] > hist[mode_ms])
            mode_ms = i;
    for (i = 0; i < n; i++)
        if (iv[i] * 2 > (ULONG)mode_ms * 3000UL)
            longer++;
    printf("  %s: %d intervals, %lu-%lu us, mean %lu us; most %d ms; %d longer than 1.5x that\n", what, n,
           (unsigned long)lo, (unsigned long)hi, (unsigned long)(sum / n), mode_ms, longer);
    printf("   ms:count");
    for (i = 0; i <= 40; i++)
        if (hist[i])
            printf(" %d:%d", i, hist[i]);
    printf("\n");
}

static void flip(struct Screen *s, int wait_disp, int way, const char *name)
{
    struct ScreenBuffer *sb[2];
    struct MsgPort *safe_port = CreateMsgPort(), *disp_port = CreateMsgPort();
    struct RastPort rp[2];
    BOOL safe = TRUE, disp = TRUE;
    ULONG t0, t, frames = 0, refused = 0, last_safe = 0, last_disp = 0, at;
    int cur = 1, i, x, old[2] = {-1, -1}, n_safe = 0, n_disp = 0;
    announce(s, way);
    sb[0] = AllocScreenBuffer(s, 0, SB_SCREEN_BITMAP);
    sb[1] = AllocScreenBuffer(s, 0, SB_COPY_BITMAP);
    if (!safe_port || !disp_port || !sb[0] || !sb[1]) {
        printf("%s: FAIL, no screen buffers (%s)\n", name,
               !sb[0] || !sb[1] ? "AllocScreenBuffer" : "CreateMsgPort");
        goto out;
    }
    for (i = 0; i < 2; i++) {
        sb[i]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safe_port;
        sb[i]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = disp_port;
        InitRastPort(&rp[i]);
        rp[i].BitMap = sb[i]->sb_BitMap;
        page(&rp[i], way);
    }
    t0 = now_us();
    while ((t = now_us() - t0) < (ULONG)seconds * 1000000UL) {
        if (!safe) {
            while (!GetMsg(safe_port))
                Wait(1UL << safe_port->mp_SigBit);
            safe = TRUE;
            at = now_us();
            if (last_safe && n_safe < MAX_IV)
                iv_safe[n_safe++] = at - last_safe;
            last_safe = at;
        }
        x = bar_x(now_us() - t0);
        bar(&rp[cur], old[cur], x);
        old[cur] = x;
        if (wait_disp && !disp) {
            while (!GetMsg(disp_port))
                Wait(1UL << disp_port->mp_SigBit);
            disp = TRUE;
            at = now_us();
            if (last_disp && n_disp < MAX_IV)
                iv_disp[n_disp++] = at - last_disp;
            last_disp = at;
        }
        if (ChangeScreenBuffer(s, sb[cur])) {
            safe = disp = FALSE;
            cur ^= 1;
            frames++;
        } else {
            refused++;
        }
    }
    printf("%s: %lu frames in %d s, %lu fps, %lu flips refused\n", name, (unsigned long)frames, seconds,
           (unsigned long)(frames / seconds), (unsigned long)refused);
    report("between safe messages", iv_safe, n_safe);
    if (wait_disp)
        report("between displayed messages", iv_disp, n_disp);
    /* The last flip's messages, before the buffers go. */
    if (!safe)
        while (!GetMsg(safe_port))
            Wait(1UL << safe_port->mp_SigBit);
    if (!disp)
        while (!GetMsg(disp_port))
            Wait(1UL << disp_port->mp_SigBit);
out:
    if (sb[1])
        FreeScreenBuffer(s, sb[1]);
    if (sb[0])
        FreeScreenBuffer(s, sb[0]);
    if (safe_port)
        DeleteMsgPort(safe_port);
    if (disp_port)
        DeleteMsgPort(disp_port);
}

/* Way n wanted: argument 1 lists them ("34"); without it, all four. */
static int wanted(const char *ways, int n)
{
    if (!ways)
        return 1;
    for (; *ways; ways++)
        if (*ways - '0' == n)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    struct Screen *s = 0;
    ULONG mode, err = 0;
    const char *ways = argc > 1 ? argv[1] : 0;
    if (argc > 2 && atoi(argv[2]) > 0)
        seconds = atoi(argv[2]);
    /* Argument 3: the task's priority while it flips (a busy task of the
     * same priority can delay its wake-up past a refresh). */
    if (argc > 3) {
        LONG pri = atoi(argv[3]);
        printf("task priority %ld (was %ld)\n", (long)pri, (long)SetTaskPri(FindTask(0), pri));
    }
    printf("ogles2 M6: presentation on the RTG screen (watch the white bar)\n");
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase || OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK, &treq.tr_node, 0)) {
        printf("needs intuition and graphics 39, cybergraphics 41, timer.device\n");
        return 20;
    }
    TimerBase = treq.tr_node.io_Device;
    mode = BestCModeIDTags(CYBRBIDTG_NominalWidth, W, CYBRBIDTG_NominalHeight, H, CYBRBIDTG_Depth, 32, TAG_DONE);
    if (mode != (ULONG)INVALID_ID)
        s = OpenScreenTags(0, SA_Depth, 24, SA_DisplayID, mode, SA_Width, W, SA_Height, H, SA_ErrorCode,
                           (ULONG)&err, SA_ShowTitle, FALSE, SA_Draggable, FALSE, SA_Quiet, TRUE, TAG_DONE);
    if (!s) {
        printf("FAIL: no 640x480 32-bit screen (mode 0x%08lx, error %lu)\n", (unsigned long)mode,
               (unsigned long)err);
        return 20;
    }
    printf("screen mode 0x%08lx\n", (unsigned long)mode);
    if (wanted(ways, 1))
        single(s, 0, 1, "1 single buffer, no wait");
    if (wanted(ways, 2))
        single(s, 1, 2, "2 single buffer, WaitTOF");
    if (wanted(ways, 3))
        flip(s, 0, 3, "3 ChangeScreenBuffer, safe message");
    if (wanted(ways, 4))
        flip(s, 1, 4, "4 ChangeScreenBuffer, safe and displayed messages");
    CloseScreen(s);
    CloseDevice(&treq.tr_node);
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    printf("done\n");
    return 0;
}
