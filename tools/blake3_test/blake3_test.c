/*
 * Checks ogles2/blake3.c against Mesa's BLAKE3 (the one v3dglslc uses), on
 * the host: every length from 0 to 5000 bytes and a few large ones, fed in
 * one piece and in uneven pieces. Built and run by `make blake3-test`.
 */
#include <stdio.h>
#include <string.h>
#include "blake3.h"          /* ours (BLAKE3_HOST_TEST) */
#include "util/blake3/blake3.h" /* Mesa's */

static unsigned char data[70000];

int main(void)
{
    static const int big[] = {8191, 8192, 8193, 16384, 31744, 65536, 69999};
    unsigned i, failures = 0, tests = 0;
    for (i = 0; i < sizeof(data); i++)
        data[i] = (unsigned char)(i % 251);
    for (i = 0; i <= 5000 + sizeof(big) / sizeof(big[0]); i++) {
        unsigned len = i <= 5000 ? i : (unsigned)big[i - 5001];
        unsigned char a[32], b[32], c[32];
        struct blake3 ours;
        blake3_hasher ref;
        unsigned off = 0, step = 1;
        blake3_hasher_init(&ref);
        blake3_hasher_update(&ref, data, len);
        blake3_hasher_finalize(&ref, a, 32);
        blake3_init(&ours);
        blake3_update(&ours, data, len);
        blake3_final(&ours, b);
        blake3_init(&ours);
        while (off < len) { /* pieces of 1, 2, 3, ... 97 bytes */
            unsigned n = len - off < step ? len - off : step;
            blake3_update(&ours, data + off, n);
            off += n;
            step = step % 97 + 1;
        }
        blake3_final(&ours, c);
        tests++;
        if (memcmp(a, b, 32) || memcmp(a, c, 32)) {
            if (failures < 10)
                printf("FAIL: length %u\n", len);
            failures++;
        }
    }
    printf("%s: %u of %u lengths match Mesa's BLAKE3\n", failures ? "FAIL" : "PASS",
           tests - failures, tests);
    return failures != 0;
}
