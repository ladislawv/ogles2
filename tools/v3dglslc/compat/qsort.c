/*
 * qsort replacing libnix's for the compiler: a stable merge sort.
 *
 * Why: libnix's quicksort compares the pivot with itself, and Mesa has
 * comparators that are inconsistent for equal elements (nir_opt_vectorize_io.c's
 * compare_intr returned -1 for x vs x, patches/0006). With such a comparator
 * quicksort partitioning runs past the array and reads unrelated memory.
 * A merge sort only ever compares two distinct positions inside the array,
 * so it stays in bounds whatever the comparator does. It is also stable, as
 * glibc's usual qsort (also a merge sort) is, which keeps the order of equal
 * elements, and therefore compiler output, closest to the Linux build.
 */
#include <stdlib.h>
#include <string.h>

typedef int (*cmp_fn)(const void *, const void *);

static void merge_pass(char *a, char *tmp, size_t n, size_t size, cmp_fn cmp)
{
    size_t width, lo;
    for (width = 1; width < n; width *= 2) {
        for (lo = 0; lo < n; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo, j = mid, k = lo;
            if (mid >= hi) {
                memcpy(tmp + lo * size, a + lo * size, (hi - lo) * size);
                continue;
            }
            while (i < mid && j < hi) {
                /* "<= 0" takes from the left run first: stable. */
                if (cmp(a + i * size, a + j * size) <= 0)
                    memcpy(tmp + k++ * size, a + i++ * size, size);
                else
                    memcpy(tmp + k++ * size, a + j++ * size, size);
            }
            if (i < mid)
                memcpy(tmp + k * size, a + i * size, (mid - i) * size);
            else if (j < hi)
                memcpy(tmp + k * size, a + j * size, (hi - j) * size);
        }
        memcpy(a, tmp, n * size);
    }
}

/* Fallback when no temporary buffer is available: stable insertion sort. */
static void insertion(char *a, size_t n, size_t size, cmp_fn cmp)
{
    char swap[64];
    size_t i, j, k;
    for (i = 1; i < n; i++) {
        for (j = i; j > 0 && cmp(a + (j - 1) * size, a + j * size) > 0; j--) {
            char *p = a + (j - 1) * size, *q = a + j * size;
            for (k = 0; k < size; k += sizeof(swap)) {
                size_t c = size - k < sizeof(swap) ? size - k : sizeof(swap);
                memcpy(swap, p + k, c);
                memcpy(p + k, q + k, c);
                memcpy(q + k, swap, c);
            }
        }
    }
}

void qsort(void *base, size_t n, size_t size, cmp_fn cmp)
{
    char *tmp;
    if (n < 2 || size == 0)
        return;
    tmp = malloc(n * size);
    if (!tmp) {
        insertion(base, n, size, cmp);
        return;
    }
    merge_pass(base, tmp, n, size, cmp);
    free(tmp);
}
