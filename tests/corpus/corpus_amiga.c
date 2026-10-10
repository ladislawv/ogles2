/*
 * corpus.h's backend on the Amiga: ogles2.library, in the window it opens.
 * Esc in that window, or Ctrl-C, ends a run early (a live run's only end).
 */
#include <stdio.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/ogles2.h>
#include "corpus.h"

struct IntuitionBase *IntuitionBase;
static void *ctx;
static struct Window *win;

int corpus_backend_open(const char *title, int width, int height, int depth, int stencil)
{
    ULONG err = 0;
    (void)title; /* the window ogles2 opens has no title */
    if (!OGLES2Open()) {
        printf("corpus: needs %s\n", OGLES2NAME);
        return 0;
    }
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_DEPTH, (ULONG)(depth ? 24 : 0),
                                OGLES2_CCT_STENCIL, (ULONG)(stencil ? 8 : 0),
                                OGLES2_CCT_PISTORM_WIDTH, (ULONG)width,
                                OGLES2_CCT_PISTORM_HEIGHT, (ULONG)height,
                                OGLES2_CCT_PISTORM_GET_WINDOW, (ULONG)&win, TAG_DONE);
    if (!ctx) {
        printf("corpus: aglCreateContextTags2 failed (error %lu)\n", (unsigned long)err);
        OGLES2Close();
        return 0;
    }
    aglMakeCurrent(ctx);
    /* Keys from the window ogles2 opened. */
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39);
    if (IntuitionBase && win)
        ModifyIDCMP(win, IDCMP_VANILLAKEY);
    return 1;
}

int corpus_backend_present(void)
{
    struct IntuiMessage *m;
    int go = 1;
    aglSwapBuffers();
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        go = 0;
    while (IntuitionBase && win && win->UserPort &&
           (m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
        if (m->Class == IDCMP_VANILLAKEY && m->Code == 27)
            go = 0;
        ReplyMsg((struct Message *)m);
    }
    return go;
}

void corpus_backend_hold(void)
{
    Delay(50); /* a second to look at the last frame */
}

void corpus_backend_close(void)
{
    if (IntuitionBase && win)
        ModifyIDCMP(win, 0);
    aglDestroyContext(ctx);
    ctx = 0;
    win = 0;
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    IntuitionBase = 0;
    OGLES2Close();
}
