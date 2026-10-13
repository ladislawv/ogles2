/*
 * The default framebuffer's surface (plan section 7.2), after PiStorm3D's
 * vid_OpenDisplay / vid_OpenWindow / vid_AdoptWindow / vid_AdoptBitMap
 * (gl/src/context.c), written for ogles2's tags:
 *
 *   OGLES2_CCT_MODEID     fullscreen: our own screen in that mode, plus a
 *                         borderless backdrop window for input
 *   OGLES2_CCT_WINDOW     the application's window: we render into an
 *                         offscreen 32-bit bitmap and ClipBlit it on swap
 *   OGLES2_CCT_BITMAP     the application's bitmap: we render into it, the
 *                         application presents it
 *   none of them          our own window on the default public screen,
 *                         OGLES2_CCT_PISTORM_WIDTH x _HEIGHT (default 640x480)
 *
 * The render target is locked only for one frame (cybergraphics.library:
 * "DON'T lock the bitmap longer than for one frame. DON'T use any library
 * calls while the bitmap is locked!"): ogles2_display_lock before the job's
 * tile stores, ogles2_display_unlock after its render has finished.
 */
#include <string.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/displayinfo.h>
#include <cybergraphx/cybergraphics.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/utility.h>
#include <libraries/ogles2.h>
#include "ogles2_internal.h"

#define NO_MODE ((ULONG)INVALID_ID)

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;

/* Opened on first use (a library that only tests state needs no display). */
BOOL ogles2_display_libs_open(void)
{
    BOOL ok;
    ObtainSemaphore(&OGLES2LibBase->lock);
    if (!IntuitionBase)
        IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39);
    if (!GfxBase)
        GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    if (!CyberGfxBase)
        CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    ok = IntuitionBase && GfxBase && CyberGfxBase;
    ReleaseSemaphore(&OGLES2LibBase->lock);
    if (!ok)
        ogles2_log("display: needs intuition 39, graphics 39 and cybergraphics 41 "
                   "(have 0x%08lx 0x%08lx 0x%08lx)", (ULONG)IntuitionBase, (ULONG)GfxBase,
                   (ULONG)CyberGfxBase);
    return ok;
}

/* Library expunge (exec runs it under Forbid; CloseLibrary is allowed). */
void ogles2_display_libs_close(void)
{
    if (CyberGfxBase) {
        CloseLibrary(CyberGfxBase);
        CyberGfxBase = 0;
    }
    if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = 0;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = 0;
    }
}

/* The store writes RGBA8 bytes; with the R/B swap they land as BGRA. Those
 * are the two 32-bit layouts V3D can produce. */
static int store_swap_for(ULONG pixfmt, int *rb_swap)
{
    if (pixfmt == PIXFMT_BGRA32) {
        *rb_swap = 1;
        return 1;
    }
    if (pixfmt == PIXFMT_RGBA32) {
        *rb_swap = 0;
        return 1;
    }
    return 0;
}

/* Probe the render bitmap once (lock, read, unlock): 4 bytes per pixel in a
 * layout the tile store can write. */
static BOOL probe_bitmap(struct OGLES2Display *d)
{
    ULONG pixfmt = 0, bprow = 0, base = 0;
    APTR lock = LockBitMapTags(d->bitmap, LBMI_PIXFMT, (ULONG)&pixfmt, LBMI_BYTESPERROW,
                               (ULONG)&bprow, LBMI_BASEADDRESS, (ULONG)&base, TAG_DONE);
    if (!lock) {
        ogles2_log("display: the render bitmap cannot be locked (not an RTG bitmap?)");
        return FALSE;
    }
    UnLockBitMap(lock);
    if (bprow < (ULONG)d->width * 4 || !store_swap_for(pixfmt, &d->rb_swap)) {
        ogles2_log("display: render bitmap pixel format %ld, %ld bytes per row for width %ld: "
                   "needs 32-bit BGRA or RGBA", pixfmt, bprow, (ULONG)d->width);
        return FALSE;
    }
    return TRUE;
}

/* An offscreen 32-bit bitmap like the window's screen, for window mode. */
static struct BitMap *alloc_render_bitmap(UWORD w, UWORD h, struct BitMap *friend_bm)
{
    struct BitMap *bm = AllocBitMap(w, h, 32, BMF_MINPLANES | BMF_DISPLAYABLE, friend_bm);
    ULONG pixfmt;
    int swap;

    if (bm) {
        pixfmt = GetCyberMapAttr(bm, CYBRMATTR_PIXFMT);
        if (store_swap_for(pixfmt, &swap))
            return bm;
        FreeBitMap(bm);
    }
    /* The friend is not a 32-bit layout (a 16-bit Workbench, say): ask for
     * BGRA32 explicitly. ClipBlit converts on presentation. */
    return AllocBitMap(w, h, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
}

static void blank_pointer(struct Window *w)
{
    static UWORD __chip blank[6]; /* sprite data: two words header, one empty line, two words end */
    if (w)
        SetPointer(w, blank, 1, 1, 0, 0);
}

static BOOL open_fullscreen(struct OGLES2Display *d, ULONG mode)
{
    ULONG err = 0, tries[3];
    int n = 0, i;
    ULONG depth;

    if (!IsCyberModeID(mode)) {
        ogles2_log("display: mode 0x%08lx is not an RTG mode", mode);
        return FALSE;
    }
    if (!store_swap_for(GetCyberIDAttr(CYBRIDATTR_PIXFMT, mode), &d->rb_swap)) {
        ogles2_log("display: mode 0x%08lx has pixel format %ld, needs 32-bit BGRA or RGBA", mode,
                   GetCyberIDAttr(CYBRIDATTR_PIXFMT, mode));
        return FALSE;
    }
    if (!d->width || !d->height) {
        d->width = (UWORD)GetCyberIDAttr(CYBRIDATTR_WIDTH, mode);
        d->height = (UWORD)GetCyberIDAttr(CYBRIDATTR_HEIGHT, mode);
    }
    /* PiStorm3D's depth ladder: 24 first (BGRA32 is 24 bits of colour), then
     * the mode's own report, then 32, retrying only on OSERR_TOODEEP. */
    depth = GetCyberIDAttr(CYBRIDATTR_DEPTH, mode);
    tries[n++] = 24;
    if (depth != 24)
        tries[n++] = depth;
    if (depth != 32)
        tries[n++] = 32;
    for (i = 0; i < n; i++) {
        err = 0;
        d->screen = OpenScreenTags(0, SA_Depth, tries[i], SA_DisplayID, mode, SA_Width,
                                   (ULONG)d->width, SA_Height, (ULONG)d->height, SA_ErrorCode,
                                   (ULONG)&err, SA_ShowTitle, FALSE, SA_Draggable, FALSE,
                                   SA_Quiet, TRUE, TAG_DONE);
        if (d->screen || err != OSERR_TOODEEP)
            break;
    }
    if (!d->screen) {
        ogles2_log("display: OpenScreen of mode 0x%08lx failed (error %ld)", mode, err);
        return FALSE;
    }
    d->own_screen = TRUE;
    d->input_window = OpenWindowTags(0, WA_CustomScreen, (ULONG)d->screen, WA_Left, 0, WA_Top, 0,
                                     WA_Width, (ULONG)d->screen->Width, WA_Height,
                                     (ULONG)d->screen->Height, WA_Flags,
                                     WFLG_ACTIVATE | WFLG_BORDERLESS | WFLG_BACKDROP |
                                         WFLG_REPORTMOUSE | WFLG_RMBTRAP,
                                     TAG_DONE);
    blank_pointer(d->input_window);
    d->bitmap = d->screen->RastPort.BitMap; /* render straight into the screen */
    return TRUE;
}

static BOOL open_window_target(struct OGLES2Display *d, struct Window *w)
{
    d->window = w;
    d->screen = w->WScreen;
    d->width = (UWORD)(w->Width - w->BorderLeft - w->BorderRight);
    d->height = (UWORD)(w->Height - w->BorderTop - w->BorderBottom);
    d->bitmap = alloc_render_bitmap(d->width, d->height, w->RPort->BitMap);
    if (!d->bitmap) {
        ogles2_log("display: no %ldx%ld offscreen bitmap", (ULONG)d->width, (ULONG)d->height);
        return FALSE;
    }
    d->own_bitmap = TRUE;
    InitRastPort(&d->rastport);
    d->rastport.BitMap = d->bitmap;
    return TRUE;
}

static BOOL open_own_window(struct OGLES2Display *d)
{
    struct Window *w;
    struct Screen *pub = LockPubScreen(0);
    if (!pub)
        return FALSE;
    w = OpenWindowTags(0, WA_PubScreen, (ULONG)pub, WA_InnerWidth, (ULONG)d->width,
                       WA_InnerHeight, (ULONG)d->height, WA_Left, 30, WA_Top, 30, WA_Title,
                       (ULONG) "ogles2", WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_Flags,
                       WFLG_ACTIVATE | WFLG_REPORTMOUSE | WFLG_RMBTRAP, TAG_DONE);
    UnlockPubScreen(0, pub); /* an open window keeps its screen open */
    if (!w) {
        ogles2_log("display: cannot open a %ldx%ld window", (ULONG)d->width, (ULONG)d->height);
        return FALSE;
    }
    d->own_window = TRUE;
    return open_window_target(d, w);
}

/* aglSetBitmap: an application-bitmap display renders into another bitmap
 * from the next frame (its size may differ). Unlocked: between jobs. */
BOOL ogles2_display_set_bitmap(struct OGLES2Display *d, struct BitMap *bitmap)
{
    struct OGLES2Display old = *d;
    if (!bitmap || !d->bitmap || d->window || d->own_screen || d->own_bitmap) {
        ogles2_log("aglSetBitmap: %s", (ULONG)(bitmap ? "the context was not created with OGLES2_CCT_BITMAP"
                                                      : "no bitmap"));
        return FALSE;
    }
    d->bitmap = bitmap;
    d->width = (UWORD)GetBitMapAttr(bitmap, BMA_WIDTH);
    d->height = (UWORD)GetBitMapAttr(bitmap, BMA_HEIGHT);
    if (d->width < 1 || d->height < 1 || d->width > 4096 || d->height > 4096 || !probe_bitmap(d)) {
        ogles2_log("aglSetBitmap: bitmap 0x%08lx (%ldx%ld) refused, the old one stays", (ULONG)bitmap,
                   (ULONG)d->width, (ULONG)d->height);
        *d = old;
        return FALSE;
    }
    return TRUE;
}

/* aglSetParams2(OGLES2_CCT_WINDOW): a window-mode display renders into
 * another window, or into the same one at its new size, from the next frame
 * (SDL does this when it recreates or resizes its window). A new offscreen
 * bitmap of the window's inner size; on failure the display is as before.
 * The old bitmap is not freed: the caller frees it (ogles2_display_drop) or
 * goes back to the old display. Between jobs (the caller finishes the
 * running one). */
BOOL ogles2_display_set_window(struct OGLES2Display *d, struct Window *w)
{
    struct OGLES2Display old = *d;
    if (!w || !d->window || d->own_window || d->own_screen || !d->own_bitmap) {
        ogles2_log("aglSetParams2: OGLES2_CCT_WINDOW: %s",
                   (ULONG)(w ? "the context was not created with OGLES2_CCT_WINDOW" : "no window"));
        return FALSE;
    }
    if (w == d->window && (UWORD)(w->Width - w->BorderLeft - w->BorderRight) == d->width &&
        (UWORD)(w->Height - w->BorderTop - w->BorderBottom) == d->height)
        return TRUE; /* the same window at the same size */
    d->bitmap = 0;
    d->own_bitmap = FALSE;
    if (!open_window_target(d, w) || d->width < 1 || d->height < 1 || d->width > 4096 ||
        d->height > 4096 || !probe_bitmap(d)) {
        ogles2_log("aglSetParams2: window 0x%08lx (%ldx%ld) refused, the old one stays", (ULONG)w,
                   (ULONG)d->width, (ULONG)d->height);
        if (d->own_bitmap && d->bitmap)
            FreeBitMap(d->bitmap);
        *d = old;
        return FALSE;
    }
    return TRUE;
}

/* The offscreen bitmap of a display left behind (ogles2_display_set_window). */
void ogles2_display_drop(struct OGLES2Display *d)
{
    if (d->own_bitmap && d->bitmap)
        FreeBitMap(d->bitmap);
    d->bitmap = 0;
}

/* ---- vsync on a screen of our own (plan 7.2) ----
 * Three screen buffers (two if the third does not fit): the GPU renders
 * into a hidden one, which ChangeScreenBuffer shows at the display's next
 * refresh. A buffer is drawn into again only after the safe message of the
 * flip that replaced it on the screen (RKRM's protocol). The next frame goes
 * into the buffer shown two flips ago, so with three it is normally free at
 * once and the CPU builds that frame while the last one waits for its
 * refresh; with two (M6, first step) every frame waited for the refresh,
 * Quake III 47 fps against 81. On the PiStorm's RTG (M6 run 9, m6_flip)
 * this shows no tearing and follows the HDMI refresh, where WaitTOF waits
 * for the chipset's 50 Hz and the single buffer tears. The display messages
 * are not asked for (no reply port).
 *
 * From the swap (and from a wait for a buffer) to the flip the task runs at
 * priority 10 at least, its own again after ChangeScreenBuffer: at priority
 * 0, behind a busy task of the same priority, a flip missed about 2
 * refreshes a second, a visible stutter (m6_flip: 20-23 of 600 flips late
 * at 0, 0-2 at 10; m6_vsync: 17-20 of 400 with only the wait raised, 3-5
 * of 418 up to the flip). The program's work before its frame's first flush
 * keeps its own priority. */
#define FLIP_WAIT_PRI 10
#define FLIP_TRIES 4 /* ChangeScreenBuffer refused: a message waited for, then again */

static void boost_end(struct OGLES2Display *d)
{
    if (d->boosted) {
        SetTaskPri(d->boosted, d->boost_pri);
        d->boosted = 0;
    }
}

static void boost(struct OGLES2Display *d)
{
    struct Task *me = FindTask(0);
    if (!d->boosted && me->tc_Node.ln_Pri < FLIP_WAIT_PRI) {
        d->boost_pri = me->tc_Node.ln_Pri;
        d->boosted = me;
        SetTaskPri(me, FLIP_WAIT_PRI);
    }
}

/* The safe messages that have come (wait: at least one more): each frees
 * the buffer its flip replaced on the screen. */
static void take_messages(struct OGLES2Display *d, BOOL wait)
{
    struct Message *m;
    int k;
    if (wait && d->msgs_due) {
        while (!(m = GetMsg(d->safe_port)))
            Wait(1UL << d->safe_port->mp_SigBit);
    } else {
        m = GetMsg(d->safe_port);
    }
    for (; m; m = GetMsg(d->safe_port)) {
        for (k = 0; k < d->nbuf; k++)
            if (m == &d->sb[k]->sb_DBufInfo->dbi_SafeMessage) {
                d->unsafe &= ~(1U << d->replaced[k]);
                break;
            }
        if (d->msgs_due)
            d->msgs_due--;
        {
            /* A missed refresh at 60 Hz (or a frame slower than one): logged at the end. */
            ULONG now = ogles2_clock_us();
            if (d->t_safe && now - d->t_safe > 25000)
                d->flips_late++;
            d->t_safe = now;
        }
    }
}

/* Before the GPU stores into the hidden buffer: until it is off the screen. */
static void flip_wait_safe(struct OGLES2Display *d)
{
    if (!d->flip)
        return;
    take_messages(d, FALSE);
    if (!(d->unsafe & (1U << d->back)))
        return;
    boost(d);
    while ((d->unsafe & (1U << d->back)) && d->msgs_due)
        take_messages(d, TRUE);
}

/* The rendered hidden buffer to the screen; the next one becomes the hidden one. */
static void flip_to_back(struct OGLES2Display *d)
{
    int tries;
    for (tries = 0; tries < FLIP_TRIES; tries++) {
        if (ChangeScreenBuffer(d->screen, d->sb[d->back])) {
            d->replaced[d->back] = d->shown;
            d->unsafe |= 1U << d->shown;
            d->shown = d->back;
            d->msgs_due++;
            d->flips++;
            d->back = (UBYTE)((d->back + 1) % d->nbuf);
            d->bitmap = d->sb[d->back]->sb_BitMap;
            return;
        }
        if (!d->msgs_due)
            break;
        take_messages(d, TRUE); /* the flip before still pending: after it */
    }
    if (!d->flip_refused) {
        d->flip_refused = TRUE;
        ogles2_log("display: ChangeScreenBuffer refused a flip (logged once): that frame is not shown");
    }
}

static void flip_free(struct OGLES2Display *d)
{
    int k;
    for (k = d->nbuf - 1; k >= 0; k--)
        if (d->sb[k])
            FreeScreenBuffer(d->screen, d->sb[k]);
    if (d->safe_port)
        DeleteMsgPort(d->safe_port);
    for (k = 0; k < 3; k++)
        d->sb[k] = 0;
    d->safe_port = 0;
    d->nbuf = 0;
}

static void flip_stop(struct OGLES2Display *d)
{
    if (!d->flip)
        return;
    while (d->msgs_due)
        take_messages(d, TRUE);
    ogles2_log("display: vsync: %lu flips, %lu of them over 25 ms after the one before", d->flips,
               d->flips_late);
    /* The screen's own bitmap shown again before the buffers go. */
    if (d->shown != 0 && ChangeScreenBuffer(d->screen, d->sb[0])) {
        d->msgs_due++;
        while (d->msgs_due)
            take_messages(d, TRUE);
    }
    boost_end(d);
    flip_free(d);
    d->flip = FALSE;
    d->bitmap = d->screen->RastPort.BitMap;
}

static BOOL flip_start(struct OGLES2Display *d)
{
    int k;
    if (d->flip || !d->own_screen)
        return d->flip;
    d->safe_port = CreateMsgPort();
    d->sb[0] = AllocScreenBuffer(d->screen, 0, SB_SCREEN_BITMAP);
    d->nbuf = 1;
    while (d->nbuf < 3 && (d->sb[d->nbuf] = AllocScreenBuffer(d->screen, 0, SB_COPY_BITMAP)) != 0)
        d->nbuf++;
    if (!d->safe_port || !d->sb[0] || d->nbuf < 2) {
        flip_free(d);
        ogles2_log("display: no second screen buffer: vsync waits for the vertical blank (WaitTOF)");
        return FALSE;
    }
    for (k = 0; k < d->nbuf; k++) {
        d->sb[k]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = d->safe_port;
        d->sb[k]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = 0;
        d->replaced[k] = 0;
    }
    d->shown = 0;
    d->unsafe = 0;
    d->msgs_due = 0;
    d->back = 1;
    d->bitmap = d->sb[1]->sb_BitMap;
    d->flips = d->flips_late = d->t_safe = 0;
    d->flip = TRUE;
    ogles2_log("display: vsync: %ld screen buffers (ChangeScreenBuffer)", (LONG)d->nbuf);
    return TRUE;
}

/* aglSwapBuffers with vsync: raised from here to the flip (see above). */
void ogles2_display_swap_begin(struct OGLES2Display *d)
{
    if (d->flip)
        boost(d);
}

/* aglSetParams2's VSYNC (the caller has finished the context's jobs: no
 * render is writing either buffer). */
void ogles2_display_set_vsync(struct OGLES2Display *d, BOOL on)
{
    d->vsync = on;
    if (!d->own_screen)
        return;
    if (on)
        flip_start(d);
    else
        flip_stop(d);
}

void ogles2_display_close(struct OGLES2Display *d)
{
    ogles2_display_unlock(d);
    if (d->flip)
        flip_stop(d);
    if (d->own_bitmap && d->bitmap)
        FreeBitMap(d->bitmap);
    if (d->input_window) {
        ClearPointer(d->input_window);
        CloseWindow(d->input_window); /* before its screen */
    }
    if (d->own_window && d->window)
        CloseWindow(d->window);
    if (d->own_screen && d->screen)
        CloseScreen(d->screen);
    memset(d, 0, sizeof(*d));
}

BOOL ogles2_display_open(struct OGLES2Display *d, struct TagItem *tags, ULONG *why)
{
    struct Window *window = (struct Window *)GetTagData(OGLES2_CCT_WINDOW, 0, tags);
    struct BitMap *bitmap = (struct BitMap *)GetTagData(OGLES2_CCT_BITMAP, 0, tags);
    ULONG mode = GetTagData(OGLES2_CCT_MODEID, NO_MODE, tags);
    BOOL ok;

    memset(d, 0, sizeof(*d));
    if (mode == NO_MODE)
        mode = GetTagData(OGLES2_CCT_CONTEXT_FOR_MODEID, NO_MODE, tags);
    d->width = (UWORD)GetTagData(OGLES2_CCT_PISTORM_WIDTH, 0, tags);
    d->height = (UWORD)GetTagData(OGLES2_CCT_PISTORM_HEIGHT, 0, tags);
    d->vsync = GetTagData(OGLES2_CCT_VSYNC, FALSE, tags) ? TRUE : FALSE;
    *why = OGLES2_CCE_NO_DISPLAY;

    if (!ogles2_display_libs_open())
        return FALSE;
    if ((window != 0) + (bitmap != 0) + (mode != NO_MODE) > 1) {
        ogles2_log("display: more than one of WINDOW, BITMAP and MODEID given");
        *why = OGLES2_CCE_BAD_TAGS;
        return FALSE;
    }
    if (bitmap) {
        d->bitmap = bitmap;
        d->width = (UWORD)GetBitMapAttr(bitmap, BMA_WIDTH);
        d->height = (UWORD)GetBitMapAttr(bitmap, BMA_HEIGHT);
        ok = TRUE;
    } else if (window) {
        ok = open_window_target(d, window);
    } else if (mode != NO_MODE) {
        ok = open_fullscreen(d, mode);
    } else {
        if (!d->width)
            d->width = 640;
        if (!d->height)
            d->height = 480;
        ok = open_own_window(d);
    }
    if (ok)
        ok = d->width >= 1 && d->height >= 1 && d->width <= 4096 && d->height <= 4096 &&
             probe_bitmap(d);
    if (!ok) {
        ogles2_display_close(d);
        return FALSE;
    }
    ogles2_log("display: %s, %ldx%ld, %s", (ULONG)(bitmap ? "application bitmap" :
                                                window ? "application window" :
                                                d->own_screen ? "fullscreen" : "own window"),
               (ULONG)d->width, (ULONG)d->height, (ULONG)(d->rb_swap ? "BGRA" : "RGBA"));
    if (d->own_screen && d->vsync)
        flip_start(d);
    *why = OGLES2_CCE_OK;
    return TRUE;
}

BOOL ogles2_display_lock(struct OGLES2Display *d)
{
    if (d->lock)
        return TRUE;
    flip_wait_safe(d); /* the hidden buffer no longer shown */
    d->lock = LockBitMapTags(d->bitmap, LBMI_BYTESPERROW, (ULONG)&d->bprow, LBMI_BASEADDRESS,
                             (ULONG)&d->base, TAG_DONE);
    return d->lock != 0;
}

/* The GPU writes the render bitmap by DMA, past the CPU's caches: a line
 * the CPU read before keeps the previous frame's bytes (A1200, m4_setbitmap:
 * one pixel of a frame stale now and then; PiStorm3D's context.c names the
 * same "stale 64-byte CPU cache lines of the previous frame"). Where the CPU
 * reads the bitmap -- the application's own, or the window's copy -- each
 * job cleans its lines before the render (job.c cleans the whole data
 * cache then, M6) and invalidates them after, while locked (exec's rule for
 * a device writing memory). A fullscreen screen is read by the display, not
 * the CPU: ogles2_display_read invalidates what it reads. */
void ogles2_display_dma(struct OGLES2Display *d, BOOL after)
{
    ULONG len;
    if (!d->lock || d->own_screen)
        return;
    len = d->bprow * d->height;
    if (after)
        CachePostDMA((APTR)d->base, &len, 0);
    else
        CachePreDMA((APTR)d->base, &len, 0);
}

void ogles2_display_unlock(struct OGLES2Display *d)
{
    if (d->lock) {
        UnLockBitMap(d->lock);
        d->lock = 0;
    }
}

void ogles2_display_read(struct OGLES2Display *d, UWORD x, UWORD y, UWORD w, UWORD h, UBYTE *dst,
                         ULONG stride)
{
    struct RastPort rp;
    UWORD r;
    if (d->own_screen && h && ogles2_display_lock(d)) {
        /* The rows y .. y+h-1, bottom-up: in memory from the top one. */
        ULONG len = d->bprow * h;
        CachePostDMA((APTR)(d->base + d->bprow * (ULONG)(d->height - (y + h))), &len, 0);
        ogles2_display_unlock(d);
    }
    InitRastPort(&rp);
    rp.BitMap = d->bitmap;
    for (r = 0; r < h; r++)
        ReadPixelArray(dst + r * stride, 0, 0, stride, &rp, x, (UWORD)(d->height - 1 - (y + r)), w, 1,
                       RECTFMT_RGBA);
}

/* After the frame's render has finished and the bitmap is unlocked. */
void ogles2_display_present(struct OGLES2Display *d)
{
    struct Window *w = d->window;
    if (d->flip) {
        /* The hidden buffer shown at the next refresh; the next frame goes
         * into the next buffer once it is off the screen (ogles2_display_lock). */
        flip_to_back(d);
        boost_end(d);
        return;
    }
    if (d->vsync)
        WaitTOF();
    if (!w || !d->own_bitmap)
        return; /* fullscreen draws into the screen; an application bitmap is the
                 * application's to present */
    /* A GimmeZeroZero window's RPort is its inner area already (SDL opens
     * its windows so): its origin is inside the borders. */
    if (w->Flags & WFLG_GIMMEZEROZERO)
        ClipBlit(&d->rastport, 0, 0, w->RPort, 0, 0, d->width, d->height, 0xC0);
    else
        ClipBlit(&d->rastport, 0, 0, w->RPort, w->BorderLeft, w->BorderTop,
                 w->Width - w->BorderLeft - w->BorderRight, w->Height - w->BorderTop - w->BorderBottom,
                 0xC0);
}
