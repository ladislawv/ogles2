/*
 * glmark2 on AmigaOS 3 (ports/glmark2): the native state over
 * ogles2.library. create_window makes the context, and with it the window
 * ogles2 opens on the Workbench screen (depth 24, stencil 8, no vsync: the
 * scores are not capped by the refresh rate). Esc in that window, or
 * Ctrl-C, ends the run.
 */
#include "native-state-amiga.h"
#include "log.h"

#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <libraries/ogles2.h>

/* libogles2.a (built by the library's own toolchain; plain C calls). */
extern "C" {
BOOL OGLES2Open(void);
void OGLES2Close(void);
void *aglCreateContextTags2(ULONG *errcode, ...);
void aglSetParamsTags2(ULONG tag1, ...);
void aglDestroyContext(void *context);
void aglMakeCurrent(void *context);
}

struct IntuitionBase *IntuitionBase;

NativeStateAmiga::NativeStateAmiga() :
    ctx_(nullptr), win_(nullptr), lib_open_(false), quit_(false)
{
}

NativeStateAmiga::~NativeStateAmiga()
{
    destroy_window();
    if (lib_open_)
        OGLES2Close();
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    IntuitionBase = nullptr;
}

bool
NativeStateAmiga::init_display()
{
    if (!IntuitionBase)
        IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    if (!IntuitionBase) {
        Log::error("intuition.library 39 did not open\n");
        return false;
    }
    if (!lib_open_ && !OGLES2Open()) {
        Log::error("ogles2.library (1.1 or newer) did not open\n");
        return false;
    }
    lib_open_ = true;
    return true;
}

void*
NativeStateAmiga::display()
{
    return nullptr;
}

void
NativeStateAmiga::destroy_window()
{
    if (!ctx_)
        return;
    if (win_)
        ModifyIDCMP(win_, 0);
    aglDestroyContext(ctx_); /* no longer current either */
    ctx_ = nullptr;
    win_ = nullptr;
}

bool
NativeStateAmiga::create_window(WindowProperties const& properties)
{
    ULONG err = 0, w = 0, h = 0;

    if (properties.fullscreen) {
        Log::error("--fullscreen is not supported: give the size with -s WxH\n");
        return false;
    }
    if (properties.width <= 0 || properties.height <= 0) {
        Log::error("Invalid surface size\n");
        return false;
    }
    destroy_window();
    ctx_ = aglCreateContextTags2(&err, OGLES2_CCT_PISTORM_WIDTH, (ULONG)properties.width,
                                 OGLES2_CCT_PISTORM_HEIGHT, (ULONG)properties.height,
                                 OGLES2_CCT_DEPTH, 24UL, OGLES2_CCT_STENCIL, 8UL,
                                 OGLES2_CCT_VSYNC, (ULONG)FALSE,
                                 OGLES2_CCT_PISTORM_GET_WINDOW, (ULONG)&win_, TAG_DONE);
    if (!ctx_) {
        Log::error("aglCreateContextTags2 failed (error %lu)\n", (unsigned long)err);
        return false;
    }
    aglMakeCurrent(ctx_);
    aglSetParamsTags2(OGLES2_CCT_GET_WIDTH, (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h, TAG_DONE);
    properties_ = properties;
    properties_.width = (int)w;
    properties_.height = (int)h;
    if (win_)
        ModifyIDCMP(win_, IDCMP_VANILLAKEY);
    return true;
}

void*
NativeStateAmiga::window(WindowProperties& properties)
{
    properties = properties_;
    return ctx_;
}

void
NativeStateAmiga::visible(bool /*v*/)
{
}

bool
NativeStateAmiga::should_quit()
{
    struct IntuiMessage *m;

    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        quit_ = true;
    while (win_ && win_->UserPort && (m = (struct IntuiMessage *)GetMsg(win_->UserPort))) {
        if (m->Class == IDCMP_VANILLAKEY && m->Code == 27)
            quit_ = true;
        ReplyMsg((struct Message *)m);
    }
    return quit_;
}

void
NativeStateAmiga::flip()
{
}
