/*
 * dEQP-GLES2 on AmigaOS 3 (ports/deqp): delibs' OS layer where its Unix one
 * needs what libnix does not have (pthreads, a monotonic clock, dlopen).
 * deqp-gles2 runs every test in its one task, so mutexes, semaphores and
 * thread-locals are that task's own; starting a thread fails (only the
 * watchdog, --deqp-watchdog, would start one).
 */
#include "deThread.h"
#include "deMutex.h"
#include "deSemaphore.h"
#include "deThreadLocal.h"
#include "deClock.h"
#include "deDynamicLibrary.h"
#include "deMemory.h"

#include <stdlib.h>
#include <time.h>
#include <dos/dosextens.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

/* Clock: timer.device's E-clock (about 709 kHz; never steps back), its base
 * dos.library's own timer request's device, as libnix's time functions take
 * it. libnix's clock_gettime ticks at 50 Hz. */
uint64_t deGetMicroseconds(void)
{
    struct Device *TimerBase = DOSBase->dl_TimeReq->tr_node.io_Device;
    struct EClockVal ev;
    unsigned long freq = ReadEClock(&ev);
    uint64_t ticks = ((uint64_t)ev.ev_hi << 32) | ev.ev_lo;
    return ticks / freq * 1000000u + ticks % freq * 1000000u / freq;
}

uint64_t deGetTime(void)
{
    return (uint64_t)time(NULL);
}

/* Threads: none. */
void deSleep(uint32_t milliseconds)
{
    Delay((milliseconds + 19) / 20);
}

void deYield(void)
{
}

deThread deThread_create(deThreadFunc func, void *arg, const deThreadAttributes *attributes)
{
    DE_UNREF(func);
    DE_UNREF(arg);
    DE_UNREF(attributes);
    return 0;
}

bool deThread_join(deThread thread)
{
    DE_UNREF(thread);
    return false;
}

void deThread_destroy(deThread thread)
{
    DE_UNREF(thread);
}

uint32_t deGetNumTotalPhysicalCores(void)
{
    return 1;
}

uint32_t deGetNumTotalLogicalCores(void)
{
    return 1;
}

uint32_t deGetNumAvailableLogicalCores(void)
{
    return 1;
}

/* Mutexes: the one task never waits on itself. */
deMutex deMutex_create(const deMutexAttributes *attributes)
{
    DE_UNREF(attributes);
    return 1;
}

void deMutex_destroy(deMutex mutex)
{
    DE_UNREF(mutex);
}

void deMutex_lock(deMutex mutex)
{
    DE_UNREF(mutex);
}

bool deMutex_tryLock(deMutex mutex)
{
    DE_UNREF(mutex);
    return true;
}

void deMutex_unlock(deMutex mutex)
{
    DE_UNREF(mutex);
}

/* Semaphores: a count; waiting on 0 would wait forever. */
deSemaphore deSemaphore_create(int initialValue, const deSemaphoreAttributes *attributes)
{
    int *count = (int *)malloc(sizeof(int));
    DE_UNREF(attributes);
    if (count)
        *count = initialValue;
    return (deSemaphore)count;
}

void deSemaphore_destroy(deSemaphore semaphore)
{
    free((int *)semaphore);
}

void deSemaphore_increment(deSemaphore semaphore)
{
    ++*(int *)semaphore;
}

void deSemaphore_decrement(deSemaphore semaphore)
{
    DE_ASSERT(*(int *)semaphore > 0);
    --*(int *)semaphore;
}

bool deSemaphore_tryDecrement(deSemaphore semaphore)
{
    if (*(int *)semaphore <= 0)
        return false;
    --*(int *)semaphore;
    return true;
}

/* Thread-locals: one slot each. */
deThreadLocal deThreadLocal_create(void)
{
    return (deThreadLocal)calloc(1, sizeof(void *));
}

void deThreadLocal_destroy(deThreadLocal threadLocal)
{
    free((void **)threadLocal);
}

void *deThreadLocal_get(deThreadLocal threadLocal)
{
    return *(void **)threadLocal;
}

void deThreadLocal_set(deThreadLocal threadLocal, void *value)
{
    *(void **)threadLocal = value;
}

/* GCC's __sync builtins (de::SharedPtr's reference counts, deAtomic.h) are
 * calls on the 68k, and libgcc has none: plain arithmetic for the one task.
 * The names are the assembler's (the ABI's leading underscore), as GCC does
 * not allow the builtins' own names to be defined. */
unsigned int deqp_sync_add_and_fetch_4(volatile void *p, unsigned int v) __asm__("___sync_add_and_fetch_4");
unsigned int deqp_sync_sub_and_fetch_4(volatile void *p, unsigned int v) __asm__("___sync_sub_and_fetch_4");
unsigned int deqp_sync_fetch_and_add_4(volatile void *p, unsigned int v) __asm__("___sync_fetch_and_add_4");
unsigned int deqp_sync_val_compare_and_swap_4(volatile void *p, unsigned int o, unsigned int n)
    __asm__("___sync_val_compare_and_swap_4");

unsigned int deqp_sync_add_and_fetch_4(volatile void *p, unsigned int v)
{
    return *(volatile unsigned int *)p += v;
}

unsigned int deqp_sync_sub_and_fetch_4(volatile void *p, unsigned int v)
{
    return *(volatile unsigned int *)p -= v;
}

unsigned int deqp_sync_fetch_and_add_4(volatile void *p, unsigned int v)
{
    unsigned int old = *(volatile unsigned int *)p;
    *(volatile unsigned int *)p = old + v;
    return old;
}

unsigned int deqp_sync_val_compare_and_swap_4(volatile void *p, unsigned int o, unsigned int n)
{
    unsigned int old = *(volatile unsigned int *)p;
    if (old == o)
        *(volatile unsigned int *)p = n;
    return old;
}

/* Dynamic libraries: none (the GL functions come from aglGetProcAddress). */
deDynamicLibrary *deDynamicLibrary_open(const char *fileName)
{
    DE_UNREF(fileName);
    return NULL;
}

deFunctionPtr deDynamicLibrary_getFunction(const deDynamicLibrary *library, const char *symbolName)
{
    DE_UNREF(library);
    DE_UNREF(symbolName);
    return NULL;
}

void deDynamicLibrary_close(deDynamicLibrary *library)
{
    DE_UNREF(library);
}
