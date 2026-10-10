#ifndef OGLES2_BLAKE3_H
#define OGLES2_BLAKE3_H
/* BLAKE3 hash, 32-byte output (blake3.c). */
#ifdef BLAKE3_HOST_TEST
typedef unsigned int ULONG;
typedef unsigned char UBYTE;
#else
#include <exec/types.h>
#endif

struct blake3
{
    ULONG key[8];
    ULONG chunk_cv[8];
    ULONG chunk_counter;
    UBYTE buf[64];
    ULONG buf_len;
    ULONG blocks_compressed;
    ULONG stack[54][8]; /* enough for 2^54 chunks */
    ULONG stack_len;
};

void blake3_init(struct blake3 *h);
void blake3_update(struct blake3 *h, const void *data, ULONG len);
void blake3_final(struct blake3 *h, UBYTE out[32]);
#endif
