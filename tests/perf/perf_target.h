/*
 * The Amiga half of the speed comparison (perf_gears.h): the target both
 * libraries render into (a hidden 640x480 BGRA32 bitmap, a Workbench window
 * of that inner size, or a 640x480 screen of each library's own), the clock
 * (DateStamp ticks, 1/50 s) and the result line. The bitmap measures the
 * libraries alone; the window adds each library's own copy to the window
 * (ClipBlit, both); the screen is shown as the GPU writes it.
 */
#ifndef PERF_TARGET_H
#define PERF_TARGET_H

#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include "perf_gears.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
static struct Window *pt_win;
static struct BitMap *pt_bm;

static int pt_open(const struct pg_args *a, const char *title)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase)
        return 0;
    if (a->fullscreen)
        return 1; /* the library opens the screen */
    if (a->window)
        pt_win = OpenWindowTags(0, WA_Title, (ULONG)title, WA_InnerWidth, PG_W, WA_InnerHeight, PG_H,
                                WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_Activate, TRUE,
                                WA_RMBTrap, TRUE, TAG_DONE);
    else
        pt_bm = AllocBitMap(PG_W, PG_H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    return pt_win || pt_bm;
}

static void pt_close(void)
{
    if (pt_win)
        CloseWindow(pt_win);
    if (pt_bm)
        FreeBitMap(pt_bm);
    pt_win = 0;
    pt_bm = 0;
    if (CyberGfxBase)
        CloseLibrary(CyberGfxBase);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
}

/* A 640x480 RTG mode the GPU can store into: BGRA32 or RGBA32 (ogles2's
 * fullscreen; BestCModeID may offer ARGB32). */
static ULONG pt_find_mode(void)
{
    ULONG id = INVALID_ID, best = INVALID_ID;
    while ((id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
        if (IsCyberModeID(id)) {
            ULONG f = GetCyberIDAttr(CYBRIDATTR_PIXFMT, id);
            if ((f == PIXFMT_BGRA32 || f == PIXFMT_RGBA32) && GetCyberIDAttr(CYBRIDATTR_WIDTH, id) == PG_W &&
                GetCyberIDAttr(CYBRIDATTR_HEIGHT, id) == PG_H)
                return id;
            if ((f == PIXFMT_BGRA32 || f == PIXFMT_RGBA32) && best == (ULONG)INVALID_ID &&
                GetCyberIDAttr(CYBRIDATTR_WIDTH, id) >= PG_W && GetCyberIDAttr(CYBRIDATTR_HEIGHT, id) >= PG_H)
                best = id;
        }
    }
    return best;
}

static ULONG pt_ticks(void)
{
    struct DateStamp d;
    DateStamp(&d);
    return (ULONG)d.ds_Days * 4320000UL + (ULONG)d.ds_Minute * 3000UL + (ULONG)d.ds_Tick;
}

static int pt_break(void)
{
    return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

/* After the timed frames, one frame at angle 0 on the bitmap: how many
 * pixels are mostly red, green and blue (the three gears). The two programs'
 * counts should agree; a library swapping red and blue shows as swapped
 * counts. */
static void pt_image(const char *lib)
{
    static ULONG row[PG_W];
    struct RastPort rp;
    ULONG r = 0, g = 0, b = 0, x, y;
    if (!pt_bm)
        return;
    InitRastPort(&rp);
    rp.BitMap = pt_bm;
    for (y = 0; y < PG_H; y++) {
        ReadPixelArray(row, 0, 0, PG_W * 4, &rp, 0, (UWORD)y, PG_W, 1, RECTFMT_ARGB);
        for (x = 0; x < PG_W; x++) {
            ULONG p = row[x], cr = (p >> 16) & 255, cg = (p >> 8) & 255, cb = p & 255;
            if (cr > 24 && cr > 2 * cg && cr > 2 * cb)
                r++;
            else if (cg > 24 && cg > 2 * cr && cg > 2 * cb)
                g++;
            else if (cb > 24 && cb > cr + 16 && cb > cg + 16)
                b++;
        }
    }
    printf("IMAGE  %-6s pixels mostly red %lu, green %lu, blue %lu\n", lib, (unsigned long)r,
           (unsigned long)g, (unsigned long)b);
}

/* One line, the same for both programs (the run script collects them). */
static void pt_report(const char *lib, const struct pg_args *a, ULONG frames, ULONG ticks)
{
    ULONG fps10 = ticks ? frames * 500UL / ticks : 0;
    printf("RESULT %-6s %-9s %-6s TEETH=%-2d %6d triangles: %5lu frames in %3lu.%02lu s = %4lu.%lu fps\n", lib,
           a->path, a->fullscreen ? "screen" : a->window ? "window" : "bitmap", a->mult, pg_triangles(), (unsigned long)frames,
           (unsigned long)(ticks / 50), (unsigned long)(ticks % 50 * 2), (unsigned long)(fps10 / 10),
           (unsigned long)(fps10 % 10));
}

#endif
