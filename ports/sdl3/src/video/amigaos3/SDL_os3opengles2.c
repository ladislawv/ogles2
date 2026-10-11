/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/*
 * ALTERED SOURCE (ogles2, ports/sdl3): OpenGL ES 2.0 for the AmigaOS 3
 * video driver of SDL3_68k, on ogles2.library (PiStorm, V3D 4.2). Derived
 * from AmigaOS 4's src/video/amigaos4/SDL_os4opengles2.c and its backend
 * choice (SDL_os4video.c), as ports/sdl2's for SDL2_68k: a window whose GL
 * profile is ES 2.x (SDL's GLES2 renderer asks for that) gets an ogles2
 * context; any other profile keeps the driver's minigl.library backend
 * (SDL_os3opengl.c).
 */
#include "SDL_internal.h"

#if SDL_VIDEO_DRIVER_AMIGAOS3 && defined(SDL_VIDEO_OPENGL_ES2)

#include <proto/exec.h>
#include <proto/ogles2.h> /* ogles2's libogles2.a: agl*, inline gl* */

#include "SDL_os3video.h"
#include "SDL_os3window.h"
#include "SDL_os3opengles2.h"
#if defined(SDL_VIDEO_OPENGL)
#include "SDL_os3opengl.h"
#endif

/* libogles2.a (client/ogles2_open.c) */
extern BOOL OGLES2Open(void);
extern void OGLES2Close(void);

static bool os3_gles2_open;
static int os3_gles2_swap_interval;

/* The profile SDL asks for: ES 2.x goes to ogles2.library. */
static bool OS3_GLES2_Wanted(SDL_VideoDevice *_this)
{
    return _this->gl_config.profile_mask == SDL_GL_CONTEXT_PROFILE_ES && _this->gl_config.major_version == 2;
}

static void OS3_GLES2_SetFunctions(SDL_VideoDevice *device)
{
    device->GL_UnloadLibrary = OS3_GLES2_UnloadLibrary;
    device->GL_GetProcAddress = OS3_GLES2_GetProcAddress;
    device->GL_CreateContext = OS3_GLES2_CreateContext;
    device->GL_DestroyContext = OS3_GLES2_DestroyContext;
    device->GL_MakeCurrent = OS3_GLES2_MakeCurrent;
    device->GL_SetSwapInterval = OS3_GLES2_SetSwapInterval;
    device->GL_GetSwapInterval = OS3_GLES2_GetSwapInterval;
    device->GL_SwapWindow = OS3_GLES2_SwapWindow;
}

#if defined(SDL_VIDEO_OPENGL)
static void OS3_MiniGL_SetFunctions(SDL_VideoDevice *device)
{
    device->GL_UnloadLibrary = OS3_GL_UnloadLibrary;
    device->GL_GetProcAddress = OS3_GL_GetProcAddress;
    device->GL_CreateContext = OS3_GL_CreateContext;
    device->GL_DestroyContext = OS3_GL_DestroyContext;
    device->GL_MakeCurrent = OS3_GL_MakeCurrent;
    device->GL_SetSwapInterval = OS3_GL_SetSwapInterval;
    device->GL_GetSwapInterval = OS3_GL_GetSwapInterval;
    device->GL_SwapWindow = OS3_GL_SwapWindow;
}
#endif

/* device->GL_LoadLibrary: the backend for the profile asked for, its
 * functions installed (as AmigaOS 4's OS4_GL_LoadLibrary). */
bool OS3_GL_LoadLibraryAny(SDL_VideoDevice *_this, const char *path)
{
    if (OS3_GLES2_Wanted(_this)) {
        OS3_GLES2_SetFunctions(_this);
        return OS3_GLES2_LoadLibrary(_this, path);
    }
#if defined(SDL_VIDEO_OPENGL)
    OS3_MiniGL_SetFunctions(_this);
    return OS3_GL_LoadLibrary(_this, path);
#else
    return SDL_SetError("AmigaOS3: only OpenGL ES 2.0 (ogles2.library) is available");
#endif
}

bool OS3_GLES2_LoadLibrary(SDL_VideoDevice *_this, const char *path)
{
    (void)_this;
    (void)path;
    if (os3_gles2_open) {
        return true;
    }
    if (!OGLES2Open()) {
        return SDL_SetError("AmigaOS3: cannot open ogles2.library");
    }
    os3_gles2_open = true;
    return true;
}

void OS3_GLES2_UnloadLibrary(SDL_VideoDevice *_this)
{
    (void)_this;
    if (os3_gles2_open) {
        OGLES2Close();
        os3_gles2_open = false;
    }
}

/* GL functions without the context argument (libogles2.a's thunks), as
 * SDL's GLES2 renderer and programs call them. */
SDL_FunctionPointer OS3_GLES2_GetProcAddress(SDL_VideoDevice *_this, const char *proc)
{
    void *func;
    (void)_this;
    if (!proc) {
        return NULL;
    }
    func = aglGetProcAddress(proc);
    if (!func) {
        SDL_SetError("AmigaOS3: ogles2.library has no '%s'", proc);
    }
    return (SDL_FunctionPointer)func;
}

static int inner_w(const struct Window *w)
{
    return w->Width - w->BorderLeft - w->BorderRight;
}

static int inner_h(const struct Window *w)
{
    return w->Height - w->BorderTop - w->BorderBottom;
}

SDL_GLContext OS3_GLES2_CreateContext(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *d = window ? window->internal : NULL;
    struct Window *w;
    ULONG err = 0;
    void *ctx;

    if (!d || !d->syswin) {
        SDL_SetError("AmigaOS3: SDL window has no native drawable");
        return NULL;
    }
    if (d->gl_context) {
        SDL_SetError("AmigaOS3: this SDL window already has a GL context");
        return NULL;
    }
    if (!os3_gles2_open && !OS3_GLES2_LoadLibrary(_this, NULL)) {
        return NULL;
    }
    w = d->syswin;
    if (!w->RPort || !w->RPort->BitMap || inner_w(w) <= 0 || inner_h(w) <= 0) {
        SDL_SetError("AmigaOS3: OpenGL ES 2 needs a nonempty window");
        return NULL;
    }
    /* The window's inner area, through an offscreen bitmap and ClipBlit
     * (ogles2's window mode; a fullscreen SDL window is a borderless window
     * on its own screen). */
    ctx = aglCreateContextTags2(&err,
                                OGLES2_CCT_WINDOW, (ULONG)w,
                                OGLES2_CCT_VSYNC, (ULONG)os3_gles2_swap_interval,
                                OGLES2_CCT_DEPTH, (ULONG)_this->gl_config.depth_size,
                                OGLES2_CCT_STENCIL, (ULONG)_this->gl_config.stencil_size,
                                TAG_DONE);
    if (!ctx) {
        SDL_SetError("AmigaOS3: ogles2.library could not create a context (error %lu)", (unsigned long)err);
        return NULL;
    }
    d->gl_context = (SDL_GLContext)ctx;
    d->gl_es = true;
    d->gl_width = inner_w(w);
    d->gl_height = inner_h(w);
    aglMakeCurrent(ctx);
    glViewport(0, 0, d->gl_width, d->gl_height);
    return (SDL_GLContext)ctx;
}

bool OS3_GLES2_MakeCurrent(SDL_VideoDevice *_this, SDL_Window *window, SDL_GLContext context)
{
    (void)_this;
    (void)window;
    aglMakeCurrent(context); /* NULL: none */
    return true;
}

/* The window resized: the context renders into it at its new size
 * (ogles2's aglSetParams2(OGLES2_CCT_WINDOW)); objects and state stay. The
 * window code calls this through OS3_GL_ResizeWindow (SDL_os3opengl.c). */
void OS3_GLES2_ResizeWindow(SDL_VideoDevice *_this, SDL_Window *window, int w, int h)
{
    SDL_WindowData *d = window ? window->internal : NULL;
    void *previous = OGLES2CurrentContext;
    (void)_this;
    if (!d || !d->gl_context || !d->syswin || w <= 0 || h <= 0) {
        return;
    }
    if (w != d->gl_width || h != d->gl_height) {
        aglMakeCurrent(d->gl_context);
        aglSetParamsTags2(OGLES2_CCT_WINDOW, (ULONG)d->syswin, TAG_DONE);
        d->gl_width = w;
        d->gl_height = h;
        if (previous != (void *)d->gl_context) {
            aglMakeCurrent(previous);
        }
    }
}

bool OS3_GLES2_SetSwapInterval(SDL_VideoDevice *_this, int interval)
{
    (void)_this;
    if (interval != 0 && interval != 1) {
        return SDL_SetError("AmigaOS3: ogles2.library supports swap interval 0 or 1 only");
    }
    os3_gles2_swap_interval = interval;
    if (OGLES2CurrentContext) {
        aglSetParamsTags2(OGLES2_CCT_VSYNC, (ULONG)interval, TAG_DONE);
    }
    return true;
}

bool OS3_GLES2_GetSwapInterval(SDL_VideoDevice *_this, int *interval)
{
    (void)_this;
    if (interval) {
        *interval = os3_gles2_swap_interval;
    }
    return true;
}

bool OS3_GLES2_SwapWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *d = window ? window->internal : NULL;
    if (!d || !d->gl_context || !d->syswin) {
        return SDL_SetError("AmigaOS3: SDL window has no OpenGL ES 2 context");
    }
    /* Host-owned windows resize outside SDL's IDCMP event loop. */
    OS3_GLES2_ResizeWindow(_this, window, inner_w(d->syswin), inner_h(d->syswin));
    if (OGLES2CurrentContext != (void *)d->gl_context) {
        aglMakeCurrent(d->gl_context);
    }
    /* ogles2 renders the frame and presents it (waiting for the vertical
     * blank at swap interval 1); no glFinish, which would stop the next
     * frame's CPU work overlapping this one's render. */
    aglSwapBuffers();
    return true;
}

bool OS3_GLES2_DestroyContext(SDL_VideoDevice *_this, SDL_GLContext context)
{
    SDL_Window *window;
    if (!context) {
        return true;
    }
    for (window = _this->windows; window; window = window->next) {
        SDL_WindowData *d = window->internal;
        if (d && d->gl_context == context) {
            d->gl_context = NULL;
            d->gl_es = false;
            d->gl_width = d->gl_height = 0;
            break;
        }
    }
    if (OGLES2CurrentContext == (void *)context) {
        aglMakeCurrent(NULL);
    }
    aglDestroyContext(context);
    return true;
}

/* Is this context one of ours (SDL_os3opengl.c's functions, which the
 * window and event code call directly, hand ogles2 contexts over)? */
bool OS3_GLES2_OwnsWindow(SDL_Window *window)
{
    SDL_WindowData *d = window ? window->internal : NULL;
    return d && d->gl_context && d->gl_es;
}

bool OS3_GLES2_OwnsContext(SDL_VideoDevice *_this, SDL_GLContext context)
{
    SDL_Window *window;
    for (window = _this->windows; window; window = window->next) {
        SDL_WindowData *d = window->internal;
        if (d && d->gl_context == context) {
            return d->gl_es;
        }
    }
    return false;
}

#endif /* SDL_VIDEO_DRIVER_AMIGAOS3 && SDL_VIDEO_OPENGL_ES2 */
