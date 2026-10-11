/*
 * ETC1 decoding (OES_compressed_ETC1_RGB8_texture), into the RGBA8 storage
 * every texture format uses for now (plan 7.6; V3D samples ETC1 natively,
 * which comes with the native formats). As the extension specifies: 4x4
 * texels per 64-bit big-endian block, two subblocks (side by side, or one
 * above the other with the flip bit), each a base colour (two 4-bit ones,
 * or a 5-bit one and a 3-bit signed difference) plus one of eight modifier
 * rows, the modifier per texel from two index bits. Checked against Mesa's
 * decoder on the host (tools/etc1_test, `make etc1-test`).
 */
#include "etc1.h"

static const short modifier[8][4] = {
    {2, 8, -2, -8},       {5, 17, -5, -17},     {9, 29, -9, -29},     {13, 42, -13, -42},
    {18, 60, -18, -60},   {24, 80, -24, -80},   {33, 106, -33, -106}, {47, 183, -47, -183}};

static UBYTE clamp255(int v)
{
    return (UBYTE)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* One block into dst (RGBA8, `stride` bytes a row), at most w x h texels. */
static void block(const UBYTE *b, UBYTE *dst, ULONG stride, int w, int h)
{
    ULONG hi = ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) | ((ULONG)b[2] << 8) | b[3];
    ULONG lo = ((ULONG)b[4] << 24) | ((ULONG)b[5] << 16) | ((ULONG)b[6] << 8) | b[7];
    int base[2][3], k, x, y, cw[2], flip = (int)(hi & 1);
    if (hi & 2) { /* differential: a 5-bit base and a 3-bit signed difference */
        for (k = 0; k < 3; k++) {
            int c = (int)((hi >> (27 - 8 * k)) & 31);
            int d = (int)((hi >> (24 - 8 * k)) & 7);
            UBYTE c2;
            if (d >= 4)
                d -= 8;
            /* A sum outside 0..31 is undefined in ETC1 (ETC2 uses it for
             * other modes); computed in a byte as Mesa's decoder does. */
            c2 = (UBYTE)(c + d);
            base[0][k] = (c << 3) | (c >> 2);
            base[1][k] = (UBYTE)((c2 << 3) | (c2 >> 2));
        }
    } else { /* individual: two 4-bit bases */
        for (k = 0; k < 3; k++) {
            int c1 = (int)((hi >> (28 - 8 * k)) & 15), c2 = (int)((hi >> (24 - 8 * k)) & 15);
            base[0][k] = (c1 << 4) | c1;
            base[1][k] = (c2 << 4) | c2;
        }
    }
    cw[0] = (int)((hi >> 5) & 7);
    cw[1] = (int)((hi >> 2) & 7);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int i = x * 4 + y; /* texels are numbered down the columns */
            int sub = flip ? (y >= 2) : (x >= 2);
            int idx = (int)((((lo >> (i + 16)) & 1) << 1) | ((lo >> i) & 1));
            int m = modifier[cw[sub]][idx];
            UBYTE *d = dst + (ULONG)y * stride + (ULONG)x * 4;
            d[0] = clamp255(base[sub][0] + m);
            d[1] = clamp255(base[sub][1] + m);
            d[2] = clamp255(base[sub][2] + m);
            d[3] = 255;
        }
}

void ogles2_etc1_decode(const UBYTE *data, ULONG width, ULONG height, UBYTE *rgba)
{
    ULONG bw = (width + 3) / 4, bh = (height + 3) / 4, bx, by;
    for (by = 0; by < bh; by++)
        for (bx = 0; bx < bw; bx++) {
            int w = width - bx * 4 < 4 ? (int)(width - bx * 4) : 4;
            int h = height - by * 4 < 4 ? (int)(height - by * 4) : 4;
            block(data + (by * bw + bx) * 8, rgba + (by * 4 * width + bx * 4) * 4, width * 4, w, h);
        }
}
