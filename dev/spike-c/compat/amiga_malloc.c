/*
 * malloc family with 8-byte alignment, replacing libnix's for the compiler.
 *
 * Mesa's ralloc gives its header alignas(8) (16 on 64-bit) and assumes malloc
 * returns memory at least that aligned, as glibc does; libnix's malloc does not
 * guarantee it, and ralloc's linear allocator then fails its alignment checks.
 * The pointer is aligned explicitly rather than relying on the pool's
 * granularity (exec rounds to 8 bytes, but vamos's emulated pools only to 4):
 * one spare 8-byte unit is allocated, and the block's start and size are kept
 * in the 8 bytes just below the returned pointer.
 *
 * Spike version: one pool, created on first use and deleted at exit, so
 * nothing leaks when a test program ends. Not task-safe (the compiler is
 * single-threaded). The real module takes its memory from ogles2.library
 * through a call table instead (plan section 8.4).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>

#define HDR 8u
#define PUDDLE (256u * 1024u)
#define THRESH (64u * 1024u)

static APTR pool;
static int pool_gone; /* set once pool_exit has run */

static void pool_exit(void)
{
    if (pool) {
        DeletePool(pool);
        pool = NULL;
    }
    pool_gone = 1;
}

static int pool_init(void)
{
    if (pool)
        return 1;
    pool = CreatePool(MEMF_ANY, PUDDLE, THRESH);
    if (!pool)
        return 0;
    atexit(pool_exit); /* reached once: after pool_exit no new pool is made */
    return 1;
}

/* Layout: raw ... [raw pointer | 1 if AllocVec][size n] | user (8-aligned) ...
 * Exit-time code (C++ destructors, libnix's own exit) can still allocate after
 * pool_exit. Those few blocks come from AllocVec instead of a new pool, since
 * registering another atexit handler during exit corrupts the exit code. */
static void *alloc_block(size_t n, ULONG flags)
{
    char *raw, *user;
    int late;
    if (n > 0x7ffffff0u - 2 * HDR) {
        errno = ENOMEM;
        return NULL;
    }
    late = pool_gone;
    if (!late && !pool_init()) {
        errno = ENOMEM;
        return NULL;
    }
    raw = late ? AllocVec(n + 2 * HDR, MEMF_ANY) : AllocPooled(pool, n + 2 * HDR);
    if (!raw) {
        errno = ENOMEM;
        return NULL;
    }
    user = (char *)(((ULONG)raw + 2 * HDR - 1) & ~(ULONG)(HDR - 1));
    if (flags & MEMF_CLEAR)
        memset(user, 0, n);
    ((ULONG *)user)[-2] = (ULONG)raw | (ULONG)late;
    ((ULONG *)user)[-1] = n;
    return user;
}

static size_t block_size(void *ptr)
{
    return ((ULONG *)ptr)[-1];
}

void *malloc(size_t n)
{
    return alloc_block(n, 0);
}

void *calloc(size_t count, size_t size)
{
    if (size && count > 0x7ffffff0u / size) {
        errno = ENOMEM;
        return NULL;
    }
    return alloc_block(count * size, MEMF_CLEAR);
}

void free(void *ptr)
{
    ULONG raw;
    if (!ptr)
        return;
    raw = ((ULONG *)ptr)[-2];
    if (raw & 1) {          /* a late AllocVec block */
        FreeVec((APTR)(raw & ~1UL));
        return;
    }
    /* Pool blocks freed after pool_exit were released by DeletePool already. */
    if (!pool)
        return;
    FreePooled(pool, (APTR)raw, block_size(ptr) + 2 * HDR);
}

void *realloc(void *ptr, size_t n)
{
    size_t old;
    void *q;
    if (!ptr)
        return malloc(n);
    if (n == 0) {
        free(ptr);
        return NULL;
    }
    old = block_size(ptr);
    if (n <= old)
        return ptr;
    q = malloc(n);
    if (!q)
        return NULL;
    memcpy(q, ptr, old);
    free(ptr);
    return q;
}
