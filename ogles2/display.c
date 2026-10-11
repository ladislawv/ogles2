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

void ogles2_display_close(struct OGLES2Display *d)
{
    ogles2_display_unlock(d);
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
 * job cleans its lines before the render and invalidates them after, while
 * locked (exec's rule for a device writing memory). A fullscreen screen is
 * read by the display, not the CPU: ogles2_display_read invalidates what it
 * reads. */
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
    if (d->vsync)
        WaitTOF();
    if (!w || !d->own_bitmap)
        return; /* fullscreen draws into the screen; an application bitmap is the
                 * application's to present */
    ClipBlit(&d->rastport, 0, 0, w->RPort, w->BorderLeft, w->BorderTop,
             w->Width - w->BorderLeft - w->BorderRight, w->Height - w->BorderTop - w->BorderBottom,
             0xC0);
}
