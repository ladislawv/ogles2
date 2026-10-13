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
#include <dos/dostags.h>
#include <string.h>
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

/* A process at priority -1 counting loop rounds: it runs only on CPU time
 * nobody else wants, so its count shows how much a way leaves idle (a wait
 * that sleeps leaves much, a busy wait none). It cannot starve anything. */
struct counter
{
    struct Message msg;
    volatile LONG stop;
    volatile ULONG n;
    struct Task *parent;
    BYTE done_bit;
};

static void counter_main(void)
{
    struct Process *me = (struct Process *)FindTask(0);
    struct counter *c;
    WaitPort(&me->pr_MsgPort);
    c = (struct counter *)GetMsg(&me->pr_MsgPort);
    ReplyMsg(&c->msg);
    while (!c->stop)
        c->n++;
    Forbid();
    Signal(c->parent, 1UL << c->done_bit);
}

static BOOL counter_start(struct counter *c)
{
    struct MsgPort *reply = CreateMsgPort();
    struct Process *p;
    memset(c, 0, sizeof(*c));
    c->parent = FindTask(0);
    c->done_bit = AllocSignal(-1);
    if (!reply || c->done_bit < 0) {
        if (reply)
            DeleteMsgPort(reply);
        if (c->done_bit >= 0)
            FreeSignal(c->done_bit);
        return FALSE;
    }
    p = CreateNewProcTags(NP_Entry, (ULONG)counter_main, NP_Name, (ULONG) "m6_flip counter", NP_Priority, -1,
                          NP_StackSize, 4096, TAG_DONE);
    if (!p) {
        DeleteMsgPort(reply);
        FreeSignal(c->done_bit);
        return FALSE;
    }
    c->msg.mn_ReplyPort = reply;
    c->msg.mn_Length = sizeof(*c);
    PutMsg(&p->pr_MsgPort, &c->msg);
    WaitPort(reply);
    GetMsg(reply);
    DeleteMsgPort(reply);
    return TRUE;
}

static ULONG counter_stop(struct counter *c)
{
    SetSignal(0, 1UL << c->done_bit);
    c->stop = 1;
    Wait(1UL << c->done_bit); /* this task waits: the counter runs and ends */
    FreeSignal(c->done_bit);
    return c->n;
}

/* Ways 1, 2, 5 and 6: straight into the displayed bitmap.
 * mode 0: as fast as it goes; 1: WaitTOF before each frame; 2: WaitBOVP;
 * 3: no drawing, only Delay (the idle baseline for the counter). */
static ULONG iv_wait[2048];
static void report(const char *what, const ULONG *iv, int n);

static void single(struct Screen *s, int mode, int way, const char *name)
{
    struct RastPort *rp = &s->RastPort;
    struct counter c;
    ULONG t0, t, frames = 0, last = 0, now, idle;
    int old = -1, x, n = 0;
    BOOL counting;
    announce(s, way);
    counting = mode != 0 && counter_start(&c);
    t0 = now_us();
    while ((t = now_us() - t0) < (ULONG)seconds * 1000000UL) {
        if (mode == 3) {
            Delay(10);
            continue;
        }
        if (mode == 1)
            WaitTOF();
        else if (mode == 2)
            WaitBOVP(&s->ViewPort);
        if (mode) {
            now = now_us();
            if (last && n < 2048)
                iv_wait[n++] = now - last;
            last = now;
        }
        x = bar_x(t);
        bar(rp, old, x);
        old = x;
        frames++;
    }
    idle = counting ? counter_stop(&c) : 0;
    if (mode == 3)
        printf("%s: the counter %lu rounds in %d s (all the CPU)\n", name, (unsigned long)idle, seconds);
    else
        printf("%s: %lu frames in %d s, %lu fps%s", name, (unsigned long)frames, seconds,
               (unsigned long)(frames / seconds), counting ? "" : "\n");
    if (counting && mode != 3)
        printf("; the counter %lu rounds (left idle)\n", (unsigned long)idle);
    if (mode == 1 || mode == 2)
        report(mode == 1 ? "between WaitTOFs" : "between WaitBOVPs", iv_wait, n);
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
    ULONG t0, t, frames = 0, refused = 0, last_safe = 0, last_disp = 0, at, csb_sum = 0, csb_max = 0;
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
        at = now_us();
        i = ChangeScreenBuffer(s, sb[cur]) != 0;
        at = now_us() - at;
        csb_sum += at;
        if (at > csb_max)
            csb_max = at;
        if (i) {
            safe = disp = FALSE;
            cur ^= 1;
            frames++;
        } else {
            refused++;
        }
    }
    printf("%s: %lu frames in %d s, %lu fps, %lu flips refused\n", name, (unsigned long)frames, seconds,
           (unsigned long)(frames / seconds), (unsigned long)refused);
    printf("  ChangeScreenBuffer itself: %lu us on average, %lu at most\n",
           (unsigned long)(frames + refused ? csb_sum / (frames + refused) : 0), (unsigned long)csb_max);
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

/* Ways 8 and 9: panning instead of ChangeScreenBuffer, which busy-waits for
 * the refresh on this RTG (17 ms a call; a priority-20 process flipping in
 * a loop froze the A1200, M6 run 9). A screen two (8) or three (9) times as
 * tall as the display: each frame is drawn into a hidden part, then
 * ScrollVPort shows it (the ViewPort's RyOffset). No waiting for the
 * display: whether the change waits for the refresh, tears or busy-waits
 * is what this shows (the time ScrollVPort takes is printed). */
static void pan(ULONG mode, int parts, int way)
{
    struct Screen *s;
    struct RastPort *rp;
    ULONG t0, t, frames = 0, err = 0, sv_sum = 0, sv_max = 0;
    int cur = 1, k, x, old[3] = {-1, -1, -1};
    s = OpenScreenTags(0, SA_Depth, 24, SA_DisplayID, mode, SA_Width, W, SA_Height, (ULONG)(H * parts),
                       SA_ErrorCode, (ULONG)&err, SA_ShowTitle, FALSE, SA_Draggable, FALSE, SA_Quiet, TRUE,
                       SA_AutoScroll, FALSE, TAG_DONE);
    if (!s) {
        printf("%d: FAIL, no %dx%d screen (error %lu)\n", way, W, H * parts, (unsigned long)err);
        return;
    }
    rp = &s->RastPort;
    for (k = 0; k < parts; k++) {
        int i;
        FillPixelArray(rp, 0, (UWORD)(k * H), W, H, BACKGROUND);
        for (i = 0; i < way; i++)
            FillPixelArray(rp, (UWORD)(24 + (i % 8) * 72), (UWORD)(k * H + 16 + (i / 8) * 56), 48, 40, 0x00FFFFFF);
    }
    s->ViewPort.RasInfo->RyOffset = 0;
    ScrollVPort(&s->ViewPort);
    Delay(50); /* the number alone for a second */
    t0 = now_us();
    while ((t = now_us() - t0) < (ULONG)seconds * 1000000UL) {
        ULONG a;
        x = bar_x(t);
        if (old[cur] != x) {
            if (old[cur] >= 0)
                FillPixelArray(rp, (UWORD)old[cur], (UWORD)(cur * H + TOP), BAR, H - TOP, BACKGROUND);
            FillPixelArray(rp, (UWORD)x, (UWORD)(cur * H + TOP), BAR, H - TOP, 0x00FFFFFF);
            old[cur] = x;
        }
        a = now_us();
        s->ViewPort.RasInfo->RyOffset = (WORD)(cur * H);
        ScrollVPort(&s->ViewPort);
        a = now_us() - a;
        sv_sum += a;
        if (a > sv_max)
            sv_max = a;
        frames++;
        cur = (cur + 1) % parts;
    }
    printf("%d pan, %d parts: %lu frames in %d s, %lu fps; ScrollVPort %lu us on average, %lu at most\n", way,
           parts, (unsigned long)frames, seconds, (unsigned long)(frames / seconds),
           (unsigned long)(frames ? sv_sum / frames : 0), (unsigned long)sv_max);
    CloseScreen(s);
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
    if (ways && wanted(ways, 6))
        single(s, 3, 6, "6 idle (Delay only), the baseline");
    if (ways && wanted(ways, 5))
        single(s, 2, 5, "5 single buffer, WaitBOVP");
    if (wanted(ways, 3))
        flip(s, 0, 3, "3 ChangeScreenBuffer, safe message");
    if (wanted(ways, 4))
        flip(s, 1, 4, "4 ChangeScreenBuffer, safe and displayed messages");
    CloseScreen(s);
    if (ways && wanted(ways, 8))
        pan(mode, 2, 8);
    if (ways && wanted(ways, 9))
        pan(mode, 3, 9);
    CloseDevice(&treq.tr_node);
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    printf("done\n");
    return 0;
}
