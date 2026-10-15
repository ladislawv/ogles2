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
 *
 * Licence: this file adapts PiStorm3D's code, so it is under the Hyperion
 * MiniGL Open Source License (LICENSE, section 1).
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

/* ---- vsync (plan 7.2) ----
 * The swap waits for the display's refresh and then lets the frame out: on
 * a screen of our own the frame's render is submitted right after the wait
 * (frame.c) and the GPU writes the screen from the top, faster than the
 * display scans it out (a 640x480 frame in 1-4 ms against 16.7), so no
 * tearing, and the CPU builds the next frame while the GPU renders this
 * one; in a window the copy (ClipBlit) follows the wait.
 *
 * Why this way (M6 run 9, m6_flip, the developer watching): WaitTOF and
 * WaitBOVP follow the HDMI refresh (16.7 ms) and sleep (a priority -1
 * counter kept 96% of an idle system's rounds through them).
 * ChangeScreenBuffer and ScrollVPort take a whole refresh per call, at times
 * busy-waiting it (a priority-20 process flipping in a loop froze the
 * A1200; Quake III with screen buffers 60 fps falling to 30; later runs
 * flipped only every second refresh, 30 fps).
 *
 * The task waits at priority 10 at least, until its frame is submitted:
 * woken at priority 0 behind a busy task of the same priority, it was late
 * for about 2 refreshes a second (m6_flip, 20-23 of 600 flips late at 0,
 * 0-2 at 10). */
#define REFRESH_WAIT_PRI 10

void ogles2_display_wait_refresh(struct OGLES2Display *d)
{
    struct Task *me = FindTask(0);
    if (!d->boosted && me->tc_Node.ln_Pri < REFRESH_WAIT_PRI) {
        d->boost_pri = me->tc_Node.ln_Pri;
        d->boosted = me;
        SetTaskPri(me, REFRESH_WAIT_PRI);
    }
    if (d->own_screen && d->screen)
        WaitBOVP(&d->screen->ViewPort); /* the screen's own refresh */
    else
        WaitTOF();
}

/* The frame submitted: the task's own priority again. */
void ogles2_display_wait_done(struct OGLES2Display *d)
{
    if (d->boosted) {
        SetTaskPri(d->boosted, d->boost_pri);
        d->boosted = 0;
    }
}

/* aglSetParams2's VSYNC. */
void ogles2_display_set_vsync(struct OGLES2Display *d, BOOL on)
{
    d->vsync = on;
}

void ogles2_display_close(struct OGLES2Display *d)
{
    ogles2_display_unlock(d);
    ogles2_display_wait_done(d);
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
    *why = OGLES2_CCE_OK;
    return TRUE;
}

BOOL ogles2_display_lock(struct OGLES2Display *d)
{
    if (d->lock)
        return TRUE;
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
    if (!w || !d->own_bitmap)
        return; /* fullscreen draws into the screen (with vsync, after the wait:
                 * frame.c); an application bitmap is the application's to present */
    if (d->vsync)
        WaitTOF(); /* the copy at the refresh */
    /* A GimmeZeroZero window's RPort is its inner area already (SDL opens
     * its windows so): its origin is inside the borders. */
    if (w->Flags & WFLG_GIMMEZEROZERO)
        ClipBlit(&d->rastport, 0, 0, w->RPort, 0, 0, d->width, d->height, 0xC0);
    else
        ClipBlit(&d->rastport, 0, 0, w->RPort, w->BorderLeft, w->BorderTop,
                 w->Width - w->BorderLeft - w->BorderRight, w->Height - w->BorderTop - w->BorderBottom,
                 0xC0);
}
