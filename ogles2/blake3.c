/*
 * BLAKE3, hash mode, 32-byte output: the program hash that names a .v3dp
 * file (v3dp/FORMAT.md, section PROGRAM). A compact implementation of the
 * BLAKE3 specification (reference_impl.rs of the BLAKE3 team, CC0/Apache-2.0),
 * written for a big-endian 68k: message words are read little-endian byte by
 * byte. Incremental: init, update any number of times, final.
 */
#include "blake3.h"

#define CHUNK_START 1
#define CHUNK_END   2
#define PARENT      4
#define ROOT        8

static const ULONG IV[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                            0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
static const UBYTE PERM[16] = {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void g(ULONG *s, int a, int b, int c, int d, ULONG mx, ULONG my)
{
    s[a] = s[a] + s[b] + mx;
    s[d] = ROTR(s[d] ^ s[a], 16);
    s[c] = s[c] + s[d];
    s[b] = ROTR(s[b] ^ s[c], 12);
    s[a] = s[a] + s[b] + my;
    s[d] = ROTR(s[d] ^ s[a], 8);
    s[c] = s[c] + s[d];
    s[b] = ROTR(s[b] ^ s[c], 7);
}

/* The compression function; out[0..7] is the new chaining value. */
static void compress(const ULONG cv[8], const ULONG block[16], ULONG counter_lo, ULONG counter_hi,
                     ULONG block_len, ULONG flags, ULONG out[8])
{
    ULONG s[16], m[16], t[16];
    int r, i;
    for (i = 0; i < 8; i++)
        s[i] = cv[i];
    s[8] = IV[0]; s[9] = IV[1]; s[10] = IV[2]; s[11] = IV[3];
    s[12] = counter_lo;
    s[13] = counter_hi;
    s[14] = block_len;
    s[15] = flags;
    for (i = 0; i < 16; i++)
        m[i] = block[i];
    for (r = 0; r < 7; r++) {
        g(s, 0, 4, 8, 12, m[0], m[1]);
        g(s, 1, 5, 9, 13, m[2], m[3]);
        g(s, 2, 6, 10, 14, m[4], m[5]);
        g(s, 3, 7, 11, 15, m[6], m[7]);
        g(s, 0, 5, 10, 15, m[8], m[9]);
        g(s, 1, 6, 11, 12, m[10], m[11]);
        g(s, 2, 7, 8, 13, m[12], m[13]);
        g(s, 3, 4, 9, 14, m[14], m[15]);
        if (r < 6) {
            for (i = 0; i < 16; i++)
                t[i] = m[PERM[i]];
            for (i = 0; i < 16; i++)
                m[i] = t[i];
        }
    }
    for (i = 0; i < 8; i++)
        out[i] = s[i] ^ s[i + 8];
}

static void words_from_bytes(const UBYTE *b, ULONG *w)
{
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (ULONG)b[4 * i] | ((ULONG)b[4 * i + 1] << 8) | ((ULONG)b[4 * i + 2] << 16) |
               ((ULONG)b[4 * i + 3] << 24);
}

static void chunk_reset(struct blake3 *h, ULONG counter)
{
    int i;
    for (i = 0; i < 8; i++)
        h->chunk_cv[i] = h->key[i];
    h->chunk_counter = counter;
    h->buf_len = 0;
    h->blocks_compressed = 0;
}

void blake3_init(struct blake3 *h)
{
    int i;
    for (i = 0; i < 8; i++)
        h->key[i] = IV[i];
    h->stack_len = 0;
    chunk_reset(h, 0);
}

/* The chunk's flags for its next block. */
static ULONG start_flag(const struct blake3 *h)
{
    return h->blocks_compressed == 0 ? CHUNK_START : 0;
}

static void parent_cv(const struct blake3 *h, const ULONG *left, const ULONG *right, ULONG *out)
{
    ULONG block[16];
    int i;
    for (i = 0; i < 8; i++) {
        block[i] = left[i];
        block[i + 8] = right[i];
    }
    compress(h->key, block, 0, 0, 64, PARENT, out);
}

/* A finished chunk's chaining value joins the tree (the reference's
 * add_chunk_chaining_value): merge while the chunk count is even. */
static void add_chunk_cv(struct blake3 *h, ULONG *cv, ULONG total_chunks)
{
    int i;
    while ((total_chunks & 1) == 0) {
        h->stack_len--;
        parent_cv(h, h->stack[h->stack_len], cv, cv);
        total_chunks >>= 1;
    }
    for (i = 0; i < 8; i++)
        h->stack[h->stack_len][i] = cv[i];
    h->stack_len++;
}

void blake3_update(struct blake3 *h, const void *data, ULONG len)
{
    const UBYTE *p = data;
    while (len) {
        /* A full chunk is finished only when more input arrives. */
        if (h->blocks_compressed == 16 - 1 && h->buf_len == 64) {
            ULONG w[16], cv[8];
            words_from_bytes(h->buf, w);
            compress(h->chunk_cv, w, h->chunk_counter, 0, 64, start_flag(h) | CHUNK_END, cv);
            add_chunk_cv(h, cv, h->chunk_counter + 1);
            chunk_reset(h, h->chunk_counter + 1);
        }
        if (h->buf_len == 64) { /* a full block, not the chunk's last */
            ULONG w[16];
            words_from_bytes(h->buf, w);
            compress(h->chunk_cv, w, h->chunk_counter, 0, 64, start_flag(h), h->chunk_cv);
            h->blocks_compressed++;
            h->buf_len = 0;
        }
        {
            ULONG take = 64 - h->buf_len;
            if (take > len)
                take = len;
            while (take--) {
                h->buf[h->buf_len++] = *p++;
                len--;
            }
        }
    }
}

void blake3_final(struct blake3 *h, UBYTE out[32])
{
    ULONG cv[8], block[16], flags, block_len, counter;
    ULONG input_cv[8];
    int i, n;

    /* The output of the chunk in progress (possibly empty). */
    for (i = (int)h->buf_len; i < 64; i++)
        h->buf[i] = 0;
    words_from_bytes(h->buf, block);
    for (i = 0; i < 8; i++)
        input_cv[i] = h->chunk_cv[i];
    flags = start_flag(h) | CHUNK_END;
    block_len = h->buf_len;
    counter = h->chunk_counter;

    /* Fold the stack from the top: each step is a parent node. */
    for (n = (int)h->stack_len; n > 0; n--) {
        compress(input_cv, block, counter, 0, block_len, flags, cv);
        for (i = 0; i < 8; i++) {
            block[i] = h->stack[n - 1][i];
            block[i + 8] = cv[i];
            input_cv[i] = h->key[i];
        }
        flags = PARENT;
        block_len = 64;
        counter = 0;
    }
    compress(input_cv, block, counter, 0, block_len, flags | ROOT, cv);
    for (i = 0; i < 8; i++) {
        out[4 * i] = (UBYTE)cv[i];
        out[4 * i + 1] = (UBYTE)(cv[i] >> 8);
        out[4 * i + 2] = (UBYTE)(cv[i] >> 16);
        out[4 * i + 3] = (UBYTE)(cv[i] >> 24);
    }
}
