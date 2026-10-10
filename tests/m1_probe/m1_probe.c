/*
 * M1 colour probe: where do full-strength clear colours turn black? (M1 runs
 * 1-3: a clear to 0xff showed 0x00 and 0xbf showed 0xc0, dither or not.)
 *   usage: m1_probe
 *
 * Into an application BGRA32 bitmap (nothing is shown), four measurements,
 * each separating one stage of the path:
 *   1. the CPU's own float to byte conversion, as the library does it;
 *   2. the readback: bytes the CPU writes, read back with ReadRGBPixel;
 *   3. GPU load and store: bytes the CPU writes, run through a frame that
 *      clears only depth (the tile buffer loads them and stores them back),
 *      then read raw;
 *   4. GPU clear: all 256 values in every channel, read raw.
 * Run with ENV:OGLES2/Probe=C to log the packed clear words of every job.
 * About 10 s.
 */
#include <stdio.h>
#include <string.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/ogles2.h>

struct GfxBase *GfxBase;
struct Library *CyberGfxBase;

#define W 256
#define H 128
static const char chan[4] = {'B', 'G', 'R', 'A'}; /* byte order in a BGRA32 pixel */

/* Raw access to the bitmap; the library has it unlocked between frames. */
static UBYTE *lock_bm(struct BitMap *bm, APTR *lock, ULONG *bprow)
{
    ULONG base = 0;
    *lock = LockBitMapTags(bm, LBMI_BASEADDRESS, (ULONG)&base, LBMI_BYTESPERROW, (ULONG)bprow,
                           TAG_DONE);
    return *lock ? (UBYTE *)base : 0;
}

/* Prints, for each channel, the values that came back changed, as in->out.
 * Returns how many changed in the first nchk channels. */
static int report(const char *what, UBYTE got[4][256], int nchk)
{
    int k, v, bad = 0;
    for (k = 0; k < 4; k++) {
        int n = 0;
        for (v = 0; v < 256; v++)
            if (got[k][v] != v)
                n++;
        if (k < nchk)
            bad += n;
        printf("     %s %c: %d of 256 values changed", what, chan[k], n);
        if (n) {
            int shown = 0;
            printf(":");
            for (v = 0; v < 256 && shown < 24; v++)
                if (got[k][v] != v) {
                    printf(" %02x>%02x", v, got[k][v]);
                    shown++;
                }
            if (n > shown)
                printf(" ...");
        }
        printf("\n");
    }
    return bad;
}

static UBYTE to_byte(float f) /* as ogles2/job.c */
{
    return (UBYTE)(f * 255.0f + 0.5f);
}

static void cpu_conversion(void)
{
    static const UBYTE probe[] = {0x00, 0x01, 0x3f, 0x40, 0x7f, 0x80, 0xbf, 0xc0, 0xfe, 0xff};
    volatile float f; /* keep the conversion at run time */
    unsigned i;
    int bad = 0;
    printf("1. CPU float to byte, (v/255)*255+0.5 truncated:");
    for (i = 0; i < sizeof(probe); i++) {
        UBYTE b;
        f = probe[i] / 255.0f;
        b = to_byte(f);
        printf(" %02x>%02x", probe[i], b);
        bad += b != probe[i];
    }
    f = 0.75f * 255.0f + 0.5f; /* 191.75 */
    printf("\n     (int)191.75 = %d, (int)255.5 = %d, (int)-1.5 = %d\n", (int)f,
           (int)(f + 63.75f), (int)(f - 193.25f));
    printf("%s: CPU conversion\n", bad ? "FAIL" : "PASS");
}

int main(void)
{
    static UBYTE got[4][256];
    struct BitMap *bm;
    struct RastPort rp;
    void *ctx;
    APTR lock;
    ULONG err = 0, bprow;
    UBYTE *base;
    int x, y, k, v, bad;

    printf("ogles2 M1 colour probe\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!GfxBase || !CyberGfxBase || !OGLES2Open()) {
        printf("needs graphics 39, cybergraphics 41 and %s %d\n", OGLES2NAME, OGLES2_VERSION);
        return 20;
    }
    cpu_conversion();

    bm = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    if (!bm) {
        printf("FAIL: allocate a %dx%d BGRA32 bitmap\n", W, H);
        return 20;
    }
    InitRastPort(&rp);
    rp.BitMap = bm;

    /* Pattern: in row y, pixel x, byte k holds (x + 64k + y) & 255, so every
     * channel sees all 256 values, in different pixels. */
#define PAT(x, y, k) ((UBYTE)((x) + 64 * (k) + (y)))
    base = lock_bm(bm, &lock, &bprow);
    if (!base) {
        printf("FAIL: lock the bitmap\n");
        return 20;
    }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            for (k = 0; k < 4; k++)
                base[y * bprow + x * 4 + k] = PAT(x, y, k);
    UnLockBitMap(lock);

    /* 2. ReadRGBPixel (the m1_clear check) against the raw bytes, row 0. */
    for (x = 0; x < W; x++) {
        ULONG argb = ReadRGBPixel(&rp, (UWORD)x, 0);
        got[0][PAT(x, 0, 0)] = (UBYTE)argb;
        got[1][PAT(x, 0, 1)] = (UBYTE)(argb >> 8);
        got[2][PAT(x, 0, 2)] = (UBYTE)(argb >> 16);
        got[3][PAT(x, 0, 3)] = (UBYTE)(argb >> 24);
    }
    printf("2. ReadRGBPixel of bytes the CPU wrote\n");
    bad = report("readback", got, 3);
    printf("%s: readback (colour; alpha is not checked)\n", bad ? "FAIL" : "PASS");

    ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, TAG_DONE);
    if (!ctx) {
        printf("FAIL: create (BITMAP tag), error %lu\n", (unsigned long)err);
        FreeBitMap(bm);
        return 20;
    }
    aglMakeCurrent(ctx);

    /* 3. A frame that clears only depth: load, store. */
    glClearColor(0, 1, 0, 1); /* must not be used */
    glClear(GL_DEPTH_BUFFER_BIT);
    aglSwapBuffers();
    glFinish();
    base = lock_bm(bm, &lock, &bprow);
    if (base) {
        int diff_rows = 0;
        for (y = 0; y < H; y++) {
            int row_bad = 0;
            for (x = 0; x < W; x++)
                for (k = 0; k < 4; k++) {
                    UBYTE b = base[y * bprow + x * 4 + k];
                    if (y == 0)
                        got[k][PAT(x, 0, k)] = b;
                    row_bad |= b != PAT(x, y, k);
                }
            diff_rows += row_bad;
        }
        UnLockBitMap(lock);
        printf("3. GPU load and store of bytes the CPU wrote (row 0; %d of %d rows differ)\n",
               diff_rows, H);
        bad = report("load/store", got, 4);
        printf("%s: load/store\n", bad ? "FAIL" : "PASS");
    } else {
        printf("FAIL: lock the bitmap after the frame\n");
    }

    /* 4. Clears: value v in R, v+85 in G, v+170 in B, v in A. */
    memset(got, 0, sizeof(got));
    for (v = 0; v < 256; v++) {
        UBYTE in[4]; /* B G R A */
        in[2] = (UBYTE)v;
        in[1] = (UBYTE)(v + 85);
        in[0] = (UBYTE)(v + 170);
        in[3] = (UBYTE)v;
        glClearColor(in[2] / 255.0f, in[1] / 255.0f, in[0] / 255.0f, in[3] / 255.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        aglSwapBuffers();
        glFinish();
        base = lock_bm(bm, &lock, &bprow);
        if (!base) {
            printf("FAIL: lock the bitmap after clear %d\n", v);
            break;
        }
        for (k = 0; k < 4; k++) {
            UBYTE a = base[k], b = base[(H - 1) * bprow + (W - 1) * 4 + k];
            got[k][in[k]] = a;
            if (a != b)
                printf("     clear %02x: corners differ in %c: %02x %02x\n", v, chan[k], a, b);
        }
        UnLockBitMap(lock);
    }
    printf("4. GPU clear, every value in every channel\n");
    bad = report("clear", got, 4);
    printf("%s: clear\n", bad ? "FAIL" : "PASS");
    printf("     GL error 0x%x\n", (unsigned)glGetError());

    aglDestroyContext(ctx);
    FreeBitMap(bm);
    OGLES2Close();
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    printf("done\n");
    return 0;
}
