/*
 * OGLES2Log: write ogles2.library's buffered log to its file now (plan
 * section 10). The file is T:ogles2.log, or the path in ENV:OGLES2/LogFile.
 * Useful while a program that uses the library is still running.
 *   usage: OGLES2Log
 */
#include <stdio.h>
#include <string.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <libraries/ogles2.h>

extern ULONG OGLES2PrivateLogFlushLVO(void); /* libogles2.a: vector -36 */

int main(void)
{
    char path[256];

    /* Any version: this only needs the log vector. */
    OGLES2Base = OpenLibrary((CONST_STRPTR)OGLES2NAME, 1);
    if (!OGLES2Base) {
        printf("OGLES2Log: cannot open %s\n", OGLES2NAME);
        return 10;
    }
    if (!OGLES2PrivateLogFlushLVO()) {
        printf("OGLES2Log: the library could not write its log\n");
        CloseLibrary(OGLES2Base);
        return 10;
    }
    CloseLibrary(OGLES2Base);
    if (GetVar((STRPTR) "OGLES2/LogFile", (STRPTR)path, sizeof(path), GVF_GLOBAL_ONLY) <= 0)
        strcpy(path, "T:ogles2.log");
    printf("OGLES2Log: log appended to %s\n", path);
    return 0;
}
