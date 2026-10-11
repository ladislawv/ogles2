/*
 * The library's log (plan section 10): the test machine has no serial
 * capture, so lines are kept in a RAM ring buffer and appended to a file.
 *
 * Writing to the file needs a DOS process outside Forbid(); ogles2_log()
 * itself only touches memory, so it is safe anywhere, including in the
 * library's Open/Close vectors.
 */
#include <stdarg.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "ogles2_internal.h"

#define LOG_SIZE 32768 /* bytes of buffered lines */
#define LINE_MAX 240
#define DEFAULT_PATH "T:ogles2.log"

static char ring[LOG_SIZE];
static ULONG head, used; /* write position, bytes held */
static ULONG lost;       /* lines dropped because the ring was full */
static ULONG seq;        /* line number since the library was loaded */

struct line {
    char text[LINE_MAX + 2];
    ULONG len;
};

/* RawDoFmt's output hook: character in d0, our data in a3. Stops at LINE_MAX. */
static void put_char(register UBYTE c __asm("d0"), register struct line *l __asm("a3"))
{
    if (c && l->len < LINE_MAX)
        l->text[l->len++] = (char)c;
}

/* DOS I/O needs a process. The explicit flush points (OGLES2Close through
 * vector -36, aglDestroyContext, the OGLES2Log command) are reached only from
 * the application's own calls, never under Forbid(), so the process check is
 * all they need. */
static BOOL caller_is_process(void)
{
    struct Task *me = FindTask(0);
    return DOSBase && me->tc_Node.ln_Type == NT_PROCESS;
}

#ifdef OGLES2_LOG_SYNC
/* A line can also be logged inside the library's Open/Close vectors, which
 * exec calls under Forbid(): the per-line flush checks the nest counters
 * (-1 outside Forbid/Disable on AmigaOS; vamos does not maintain them, so
 * this mode writes nothing there). */
static BOOL can_flush_now(void)
{
    return caller_is_process() && SysBase->TDNestCnt < 0 && SysBase->IDNestCnt < 0;
}
#endif

/* ogles2_format's output hook: a bounded buffer. */
struct fmt_buf {
    char *p;
    ULONG left; /* room for characters, the NUL excluded */
};

static void put_bounded(register UBYTE c __asm("d0"), register struct fmt_buf *b __asm("a3"))
{
    if (c && b->left) {
        *b->p++ = (char)c;
        b->left--;
    }
}

/* RawDoFmt into buf (size bytes with the NUL); 32-bit arguments as for
 * ogles2_log. Returns the length written. */
/* Probe T: one line per call (n < 0: a step, no count). */
void ogles2_trace(struct OGLES2Context *ctx, const char *what, LONG n, ULONG first)
{
    if (!(ctx->probe & OGLES2_PROBE_TRACE))
        return;
    if (n < 0)
        ogles2_log("trace: %s", (ULONG)what);
    else
        ogles2_log("trace: %s #%lu: %ld names from %lu", (ULONG)what, ++ctx->n_deletes, n, first);
}

ULONG ogles2_format(char *buf, ULONG size, const char *fmt, ...)
{
    struct fmt_buf b;
    va_list ap;
    if (!size)
        return 0;
    b.p = buf;
    b.left = size - 1;
    va_start(ap, fmt);
    RawDoFmt((STRPTR)fmt, (APTR)ap, (void (*)())put_bounded, &b);
    va_end(ap);
    *b.p = 0;
    return (ULONG)(b.p - buf);
}

BOOL ogles2_log_init(void)
{
    head = used = lost = 0;
    return TRUE;
}

void ogles2_log_term(void)
{
    head = used = 0;
}

static void ring_put(const char *s, ULONG n)
{
    ULONG i;
    if (n > LOG_SIZE)
        return;
    if (used + n > LOG_SIZE) {
        lost++;
        return; /* keep the oldest lines: the start of a problem matters most */
    }
    for (i = 0; i < n; i++)
        ring[(head + i) % LOG_SIZE] = s[i];
    head = (head + n) % LOG_SIZE;
    used += n;
}

void ogles2_log(const char *fmt, ...)
{
    struct line l;
    struct line num;
    va_list ap;
    ULONG n;

    va_start(ap, fmt);
    l.len = 0;
    RawDoFmt((STRPTR)fmt, (APTR)ap, (void (*)())put_char, &l);
    va_end(ap);
    l.text[l.len++] = '\n';

    Forbid();
    n = ++seq;
    num.len = 0;
    RawDoFmt((STRPTR) "%6lu ", &n, (void (*)())put_char, &num);
    ring_put(num.text, num.len);
    ring_put(l.text, l.len);
    Permit();

#ifdef OGLES2_LOG_SYNC
    if (can_flush_now())
        ogles2_log_flush();
#endif
}

BOOL ogles2_log_flush(void)
{
    char path[256];
    char *copy;
    ULONG n, tail, missed;
    BPTR fh;

    if (!caller_is_process())
        return FALSE;

    /* Take the lines out under Forbid(), write them outside it. */
    copy = AllocVec(LOG_SIZE + 64, MEMF_ANY);
    if (!copy)
        return FALSE;
    Forbid();
    n = used;
    tail = (head + LOG_SIZE - used) % LOG_SIZE;
    for (missed = 0; missed < n; missed++)
        copy[missed] = ring[(tail + missed) % LOG_SIZE];
    used = 0;
    missed = lost;
    lost = 0;
    Permit();

    if (missed) {
        struct line l;
        l.len = 0;
        RawDoFmt((STRPTR) "(%lu lines lost: log buffer full)\n", &missed, (void (*)())put_char, &l);
        CopyMem(l.text, copy + n, l.len);
        n += l.len;
    }

    if (n) {
        if (GetVar((STRPTR) "OGLES2/LogFile", (STRPTR)path, sizeof(path), GVF_GLOBAL_ONLY) <= 0)
            CopyMem(DEFAULT_PATH, path, sizeof(DEFAULT_PATH));
        fh = Open((STRPTR)path, MODE_READWRITE);
        if (fh) {
            Seek(fh, 0, OFFSET_END);
            Write(fh, copy, n);
            Close(fh);
        }
    }
    FreeVec(copy);
    return TRUE;
}
