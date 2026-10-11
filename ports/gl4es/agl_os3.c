/*
 * gl4es's agl* functions on AmigaOS 3 with ogles2.library (ports/gl4es),
 * in place of gl4es's src/agl/agl.c, which calls AmigaOS 4's IOGLES2
 * interface. The context list and its gl4es state are agl.c's; the calls go
 * to ogles2.library's dispatch table, keeping libogles2.a's current
 * context as its own aglMakeCurrent does (client/agl.c), so the GLES2
 * calls of the glue (ogles2_glue.c) reach the right context.
 *
 * gl4es is started here, at the first context, not by a constructor
 * (built with NO_INIT_CONSTRUCTOR), and stopped at exit.
 *
 * Derived from gl4es (MIT licence, Copyright (c) 2016-2018 Sebastien
 * Chevalier); see ports/gl4es/README.md.
 */
#include <stdlib.h>
#include <string.h>
#include <exec/types.h>
#include <utility/tagitem.h>
#include <libraries/ogles2.h>
#include <libraries/ogles2_dispatch.h>

extern const struct OGLES2Dispatch *OGLES2Dispatch; /* libogles2.a */
extern struct OGLES2Context *OGLES2CurrentContext;
BOOL OGLES2Open(void);
void OGLES2Close(void);
struct BitMap;

/* gl4es */
void *NewGLState(void *shared_glstate, int es2only);
void DeleteGLState(void *oldstate);
void ActivateGLState(void *new_glstate);
void GetHardwareExtensions(int notest);
void initialize_gl4es(void);
void close_gl4es(void);
void amiga_pre_swap(void);
void amiga_post_swap(void);

typedef struct _agl_ctx_glstate {
    void *context;
    void *glstate;
} agl_ctx_glstate;

static agl_ctx_glstate *agl_context = NULL;
static int agl_context_len = 0;
static int agl_context_cap = 0;
static void *agl_current_ctx = NULL;

/* Find (or add) a context in the list and activate its gl4es state. */
static void agl_context_find(void *ctx)
{
    int idx = 0;
    if (!ctx)
        return;
    if (!agl_context) {
        agl_context_cap = 10;
        agl_context = (agl_ctx_glstate *)calloc(agl_context_cap, sizeof(agl_ctx_glstate));
        if (!agl_context)
            return;
    }
    while (idx < agl_context_len && agl_context[idx].context != ctx)
        idx++;
    if (idx == agl_context_len && agl_context_len == agl_context_cap) {
        agl_ctx_glstate *n = (agl_ctx_glstate *)realloc(agl_context, sizeof(agl_ctx_glstate) * (agl_context_cap + 10));
        if (!n)
            return;
        agl_context = n;
        memset(agl_context + agl_context_cap, 0, sizeof(agl_ctx_glstate) * 10);
        agl_context_cap += 10;
    }
    agl_context[idx].context = ctx;
    if (idx == agl_context_len)
        ++agl_context_len;
    if (!agl_context[idx].glstate) {
        agl_context[idx].glstate = NewGLState(NULL, 0);
        GetHardwareExtensions(0); /* gl4es asks the GLES2 context what it can do */
    }
    ActivateGLState(agl_context[idx].glstate);
}

static void agl_context_remove(void *ctx)
{
    int idx = 0;
    if (!ctx || !agl_context)
        return;
    while (idx < agl_context_len && agl_context[idx].context != ctx)
        idx++;
    if (idx == agl_context_len)
        return;
    if (agl_context[idx].glstate) {
        DeleteGLState(agl_context[idx].glstate);
        agl_context[idx].glstate = NULL;
    }
    agl_context[idx].context = 0;
    while (agl_context_len && !agl_context[agl_context_len - 1].context)
        --agl_context_len;
    if (!agl_context_len) {
        agl_context_cap = 0;
        free(agl_context);
        agl_context = NULL;
    }
}

/* ---- starting and stopping gl4es ---- */

static int started;

static void stop(void)
{
    close_gl4es();
    OGLES2Close();
}

static int start(void)
{
    if (!started) {
        started = 1;
        initialize_gl4es(); /* gl/init.c: reads LIBGL_* settings, opens ogles2 (os4OpenLib) */
        atexit(stop);
    }
    return OGLES2Dispatch != 0;
}

/* ---- the agl functions ---- */

void *aglCreateContext2(ULONG *errcode, struct TagItem *tags)
{
    if (!start()) {
        if (errcode)
            *errcode = OGLES2_CCE_NO_GPU;
        return NULL;
    }
    return OGLES2Dispatch->aglCreateContext2(errcode, tags);
}

void *aglCreateContext(ULONG *errcode, struct TagItem *tags)
{
    return aglCreateContext2(errcode, tags);
}

/* GCC passes the arguments on the stack in order: the tags after errcode
 * already form a TagItem array (libogles2.a's client/agl.c does the same). */
void *aglCreateContextTags2(ULONG *errcode, ...)
{
    return aglCreateContext2(errcode, (struct TagItem *)(&errcode + 1));
}

static void make_current(void *context)
{
    OGLES2CurrentContext = (struct OGLES2Context *)context;
    OGLES2Dispatch->aglMakeCurrent((struct OGLES2Context *)context);
}

void gl4es_glue_profile(ULONG frames); /* ogles2_glue.c: GL4ES_GLUE_PROFILE */
static ULONG swaps;

void aglDestroyContext(void *context)
{
    if (!OGLES2Dispatch || !context)
        return;
    gl4es_glue_profile(swaps);
    swaps = 0;
    /* gl4es frees its objects of this context: make it current for that. */
    if (context != agl_current_ctx)
        make_current(context);
    agl_context_remove(context);
    if (context != agl_current_ctx)
        make_current(agl_current_ctx);
    else
        make_current(agl_current_ctx = NULL);
    OGLES2Dispatch->aglDestroyContext((struct OGLES2Context *)context);
}

void aglMakeCurrent(void *context)
{
    agl_current_ctx = context;
    if (!OGLES2Dispatch)
        return;
    make_current(context);
    if (context)
        agl_context_find(context);
}

void aglSetParams2(struct TagItem *tags)
{
    if (OGLES2Dispatch)
        OGLES2Dispatch->aglSetParams2(OGLES2CurrentContext, tags);
}

void aglSetParamsTags2(ULONG tag1, ...)
{
    aglSetParams2((struct TagItem *)&tag1);
}

void aglSwapBuffers(void)
{
    swaps++;
    amiga_pre_swap(); /* gl4es: pending display lists and bitmaps, its own FBO */
    if (OGLES2Dispatch)
        OGLES2Dispatch->aglSwapBuffers(OGLES2CurrentContext);
    amiga_post_swap();
}

void aglSetBitmap(struct BitMap *bitmap)
{
    if (OGLES2Dispatch)
        OGLES2Dispatch->aglSetBitmap(OGLES2CurrentContext, bitmap);
}
