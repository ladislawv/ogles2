/*
 * libogles2.a: opening the library and the per-program-image state
 * (plan section 6.2).
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/ogles2.h>

struct Library *OGLES2Base;
const struct OGLES2Dispatch *OGLES2Dispatch;
struct OGLES2Context *OGLES2CurrentContext;

/* lvo.s: the library's vectors */
extern const struct OGLES2Dispatch *OGLES2GetDispatchTableLVO(void);
extern ULONG OGLES2PrivateLogFlushLVO(void);

/* Where the library is looked for: installed (in memory, or LIBS:), then
 * beside the program, so a program can ship it in its own drawer when it is
 * not installed: PROGDIR:libs/, then PROGDIR:. One that is too old, or built
 * for another dispatch ABI, is closed and the next one tried, so a program
 * may also bring a newer one than the system has. */
static const char *const where[] = {
    OGLES2NAME,
    "PROGDIR:libs/" OGLES2NAME,
    "PROGDIR:" OGLES2NAME,
};

/* exec keeps one library of a name: an OpenLibrary of any path returns the
 * ogles2.library in memory (A1200, 2026-10-15). So before the next file is
 * tried, one that would not do is expunged, as Avail FLUSH does, if no
 * program has it open; one in use stays, and the next try gets it again. */
static void expunge_unused(void)
{
    struct Library *l;
    Forbid();
    l = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)OGLES2NAME);
    if (l && l->lib_OpenCnt == 0)
        RemLibrary(l);
    Permit();
}

BOOL OGLES2Open(void)
{
    const struct OGLES2Dispatch *d;
    unsigned i;

    if (OGLES2Base && OGLES2Dispatch)
        return TRUE;
    for (i = 0; i < sizeof(where) / sizeof(where[0]); i++) {
        /* OpenLibrary also fails for one in memory older than OGLES2_VERSION */
        if (!OGLES2Base && !(OGLES2Base = OpenLibrary((CONST_STRPTR)where[i], OGLES2_VERSION))) {
            expunge_unused();
            continue;
        }
        d = OGLES2GetDispatchTableLVO();
        /* abiVersion must be EQUAL: a different value means the entries mean
         * something else. structSize "at least mine": a newer library only
         * appends entries (plan section 6.7). */
        if (d && d->abiVersion == OGLES2_DISPATCH_ABI_VERSION && d->structSize >= sizeof(struct OGLES2Dispatch)) {
            OGLES2Dispatch = d;
            return TRUE;
        }
        CloseLibrary(OGLES2Base);
        OGLES2Base = 0;
        expunge_unused();
    }
    return FALSE;
}

void OGLES2Close(void)
{
    OGLES2Dispatch = 0;
    OGLES2CurrentContext = 0;
    if (OGLES2Base) {
        /* Write the library's log while this process may still do DOS I/O
         * (the library's Close vector runs under Forbid). */
        OGLES2PrivateLogFlushLVO();
        CloseLibrary(OGLES2Base);
        OGLES2Base = 0;
    }
}
