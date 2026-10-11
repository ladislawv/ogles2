/*
 * How long exec's cache calls take on this machine (M6, plan 7.1): the job's
 * flush calls CachePreDMA on four control-list buffers and the render
 * target, and CachePostDMA after the render. Times each call for sizes from
 * 64 bytes to 1 MB, with clean and with dirty lines, on the Pi's 1 MHz
 * system timer (the peripheral window v3dcore uses). Nothing to see.
 *   usage: cachetime
 */
#include <stdio.h>
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>

#define CLO (*(volatile ULONG *)(0xF2000000 + 0x3004))
#define ROUNDS 200

static ULONG now(void)
{
    ULONG v = CLO;
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

int main(void)
{
    static const ULONG sizes[] = {64, 4096, 65536, 262144, 1048576};
    UBYTE *buf = AllocVec(1048576 + 4096, MEMF_FAST);
    int i, k;
    if (!buf) {
        printf("no memory\n");
        return 20;
    }
    printf("exec cache calls, microseconds per call (%d calls each)\n", ROUNDS);
    printf("%9s %12s %12s %12s %12s\n", "bytes", "PreDMA", "PreDMA dirty", "PostDMA", "ClearE");
    for (k = 0; k < (int)(sizeof(sizes) / sizeof(sizes[0])); k++) {
        ULONG t0, pre = 0, pred = 0, post = 0, clr = 0, len;
        for (i = 0; i < ROUNDS; i++) {
            len = sizes[k];
            t0 = now();
            CachePreDMA(buf, &len, 0);
            pre += now() - t0;
            memset(buf, i, sizes[k] < 4096 ? sizes[k] : 4096); /* some dirty lines */
            len = sizes[k];
            t0 = now();
            CachePreDMA(buf, &len, 0);
            pred += now() - t0;
            len = sizes[k];
            t0 = now();
            CachePostDMA(buf, &len, 0);
            post += now() - t0;
            t0 = now();
            CacheClearE(buf, sizes[k], CACRF_ClearD);
            clr += now() - t0;
        }
        printf("%9lu %12lu %12lu %12lu %12lu\n", (unsigned long)sizes[k], (unsigned long)(pre / ROUNDS),
               (unsigned long)(pred / ROUNDS), (unsigned long)(post / ROUNDS), (unsigned long)(clr / ROUNDS));
    }
    {
        ULONG t0 = now(), t1;
        for (i = 0; i < 1000; i++)
            (void)now();
        t1 = now();
        printf("one read of the timer: %lu ns\n", (unsigned long)((t1 - t0)));
    }
    FreeVec(buf);
    return 0;
}
