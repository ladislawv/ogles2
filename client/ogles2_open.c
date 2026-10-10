/*
 * libogles2.a: opening the library and the per-program-image state
 * (plan section 6.2).
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/ogles2.h>

struct Library *OGLES2Base;
const struct OGLES2Dispatch *OGLES2Dispatch;
struct OGLES2Context *OGLES2CurrentContext;

/* lvo.s: the library's vectors */
extern const struct OGLES2Dispatch *OGLES2GetDispatchTableLVO(void);
extern ULONG OGLES2PrivateLogFlushLVO(void);

BOOL OGLES2Open(void)
{
    const struct OGLES2Dispatch *d;

    if (OGLES2Base && OGLES2Dispatch)
        return TRUE;
    if (!OGLES2Base) {
        OGLES2Base = OpenLibrary((CONST_STRPTR)OGLES2NAME, OGLES2_VERSION);
        if (!OGLES2Base)
            return FALSE;
    }
    d = OGLES2GetDispatchTableLVO();
    /* abiVersion must be EQUAL: a different value means the entries mean
     * something else. structSize "at least mine": a newer library only
     * appends entries (plan section 6.7). */
    if (!d || d->abiVersion != OGLES2_DISPATCH_ABI_VERSION ||
        d->structSize < sizeof(struct OGLES2Dispatch)) {
        CloseLibrary(OGLES2Base);
        OGLES2Base = 0;
        return FALSE;
    }
    OGLES2Dispatch = d;
    return TRUE;
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
