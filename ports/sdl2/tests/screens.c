/*
 * screens: Intuition's screens, one line each: size, depth (bits a pixel
 * for RTG), title. For the SDL2 tests (GitHub issue 5): during a fullscreen
 * program a 32-bit screen titled "SDL"; after it, none left behind.
 *   usage: screens [NOSDL]
 * NOSDL: the return code is 10 if a screen titled "SDL" is open, else 0.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>

struct IntuitionBase *IntuitionBase;
struct Library *CyberGfxBase;

int main(int argc, char **argv)
{
    struct Screen *s;
    ULONG lock;
    int n = 0, sdl = 0;
    char line[16][100];
    if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39)))
        return 20;
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    /* Copied under the lock, printed after it (no DOS calls while locked). */
    lock = LockIBase(0);
    for (s = IntuitionBase->FirstScreen; s && n < 16; s = s->NextScreen, n++) {
        const char *t = s->Title ? (const char *)s->Title : s->DefaultTitle ? (const char *)s->DefaultTitle : "";
        ULONG depth = CyberGfxBase && GetCyberMapAttr(s->RastPort.BitMap, CYBRMATTR_ISCYBERGFX)
                          ? GetCyberMapAttr(s->RastPort.BitMap, CYBRMATTR_BPPIX) * 8
                          : s->RastPort.BitMap->Depth;
        if (!strcmp(t, "SDL"))
            sdl++;
        snprintf(line[n], sizeof(line[n]), "%dx%d %lu bit \"%.60s\"", s->Width, s->Height, (unsigned long)depth, t);
    }
    UnlockIBase(lock);
    {
        int i;
        for (i = 0; i < n; i++)
            printf("screen %d: %s\n", i, line[i]);
    }
    printf("%d screen(s), %d titled SDL\n", n, sdl);
    if (CyberGfxBase)
        CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return argc > 1 && !strcmp(argv[1], "NOSDL") && sdl ? 10 : 0;
}
