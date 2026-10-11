/*
 * Checks ogles2/etc1.c against Mesa's ETC1 decoder (util/format/u_format_etc.c),
 * on the host: 20,000 random blocks (both modes, both flips, every modifier
 * row) and images of every size from 1x1 to 13x13, partial blocks included.
 * Built and run by `make etc1-test`.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "etc1.h" /* ours (ETC1_HOST_TEST) */

/* Mesa's (util/format/u_format_etc.h) */
void util_format_etc1_rgb8_unpack_rgba_8unorm(uint8_t *dst_row, unsigned dst_stride,
                                              const uint8_t *src_row, unsigned src_stride,
                                              unsigned width, unsigned height);

static unsigned long seed = 12345;

static unsigned char rnd(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return (unsigned char)(seed >> 16);
}

static unsigned char data[4 * 4 * 8], ours[13 * 13 * 4], ref[13 * 13 * 4];

int main(void)
{
    unsigned i, w, h, failures = 0, tests = 0;
    /* single blocks */
    for (i = 0; i < 20000; i++) {
        unsigned k;
        for (k = 0; k < 8; k++)
            data[k] = rnd();
        ogles2_etc1_decode(data, 4, 4, ours);
        util_format_etc1_rgb8_unpack_rgba_8unorm(ref, 16, data, 8, 4, 4);
        tests++;
        if (memcmp(ours, ref, 64)) {
            if (failures++ < 5)
                printf("block %u (%02x%02x%02x%02x %02x%02x%02x%02x) differs\n", i, data[0], data[1],
                       data[2], data[3], data[4], data[5], data[6], data[7]);
        }
    }
    /* images, partial blocks at the edges */
    for (w = 1; w <= 13; w++)
        for (h = 1; h <= 13; h++) {
            unsigned bw = (w + 3) / 4, bh = (h + 3) / 4, k;
            for (k = 0; k < bw * bh * 8; k++)
                data[k] = rnd();
            memset(ours, 0, sizeof(ours));
            memset(ref, 0, sizeof(ref));
            ogles2_etc1_decode(data, w, h, ours);
            util_format_etc1_rgb8_unpack_rgba_8unorm(ref, w * 4, data, bw * 8, w, h);
            tests++;
            if (memcmp(ours, ref, w * h * 4)) {
                if (failures++ < 10)
                    printf("image %ux%u differs\n", w, h);
            }
        }
    printf("%s: %u of %u decodes match Mesa's ETC1\n", failures ? "FAIL" : "PASS", tests - failures, tests);
    return failures ? 1 : 0;
}
