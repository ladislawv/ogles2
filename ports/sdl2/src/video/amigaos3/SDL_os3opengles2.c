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
 * ALTERED SOURCE (ogles2, ports/sdl2): OpenGL ES 2.0 for the AmigaOS 3
 * video driver of SDL2_68k, on ogles2.library (PiStorm, V3D 4.2). Derived
 * from AmigaOS 4's src/video/amigaos4/SDL_os4opengles2.c and its backend
 * choice (SDL_os4video.c): a window whose GL profile is ES 2.x (SDL's GLES2
 * renderer asks for that) gets an ogles2 context; any other profile keeps
 * the driver's minigl.library backend (SDL_os3opengl.c).
 */
#include "../../SDL_internal.h"

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

static int os3_gles2_open;
static int os3_gles2_swap_interval;

/* The profile SDL asks for: ES 2.x goes to ogles2.library. */
static SDL_bool OS3_GLES2_Wanted(_THIS)
{
    return _this->gl_config.profile_mask == SDL_GL_CONTEXT_PROFILE_ES && _this->gl_config.major_version == 2
               ? SDL_TRUE
               : SDL_FALSE;
}

static void OS3_GLES2_SetFunctions(SDL_VideoDevice *device)
{
    device->GL_UnloadLibrary = OS3_GLES2_UnloadLibrary;
    device->GL_GetProcAddress = OS3_GLES2_GetProcAddress;
    device->GL_CreateContext = OS3_GLES2_CreateContext;
    device->GL_DeleteContext = OS3_GLES2_DeleteContext;
    device->GL_MakeCurrent = OS3_GLES2_MakeCurrent;
    device->GL_GetDrawableSize = OS3_GLES2_GetDrawableSize;
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
    device->GL_DeleteContext = OS3_GL_DeleteContext;
    device->GL_MakeCurrent = OS3_GL_MakeCurrent;
    device->GL_GetDrawableSize = OS3_GL_GetDrawableSize;
    device->GL_SetSwapInterval = OS3_GL_SetSwapInterval;
    device->GL_GetSwapInterval = OS3_GL_GetSwapInterval;
    device->GL_SwapWindow = OS3_GL_SwapWindow;
}
#endif

/* device->GL_LoadLibrary: the backend for the profile asked for, its
 * functions installed (as AmigaOS 4's OS4_GL_LoadLibrary). */
int OS3_GL_LoadLibraryAny(_THIS, const char *path)
{
    if (OS3_GLES2_Wanted(_this)) {
        OS3_GLES2_SetFunctions(_this);
        return OS3_GLES2_LoadLibrary(_this, path);
    }
#if defined(SDL_VIDEO_OPENGL)
    OS3_MiniGL_SetFunctions(_this);
    return OS3_GL_LoadLibrary(_this, path);
#else
    return SDL_SetError("OS3: only OpenGL ES 2.0 (ogles2.library) is available");
#endif
}

int OS3_GLES2_LoadLibrary(_THIS, const char *path)
{
    (void)path;
    if (os3_gles2_open) {
        return 0;
    }
    if (!OGLES2Open()) {
        return SDL_SetError("OS3: cannot open ogles2.library");
    }
    os3_gles2_open = 1;
    return 0;
}

void OS3_GLES2_UnloadLibrary(_THIS)
{
    (void)_this;
    if (os3_gles2_open) {
        OGLES2Close();
        os3_gles2_open = 0;
    }
}

/* GL functions without the context argument (libogles2.a's thunks), as
 * SDL's GLES2 renderer and programs call them. */
void *OS3_GLES2_GetProcAddress(_THIS, const char *proc)
{
    void *func;
    (void)_this;
    if (!proc) {
        return NULL;
    }
    func = aglGetProcAddress(proc);
    if (!func) {
        SDL_SetError("OS3: ogles2.library has no '%s'", proc);
    }
    return func;
}

SDL_GLContext OS3_GLES2_CreateContext(_THIS, SDL_Window *window)
{
    OS3_WindowData *data;
    struct Window *iwin;
    ULONG err = 0;
    void *ctx;

    if (!window || !window->driverdata) {
        SDL_SetError("OS3: invalid SDL window for an OpenGL ES 2 context");
        return NULL;
    }
    data = (OS3_WindowData *)window->driverdata;
    if (data->gl_context) {
        SDL_SetError("OS3: this SDL window already has a GL context");
        return NULL;
    }
    if (!os3_gles2_open && OS3_GLES2_LoadLibrary(_this, NULL) < 0) {
        return NULL;
    }
    iwin = data->window;
    if (!iwin || !iwin->RPort || !iwin->RPort->BitMap) {
        SDL_SetError("OS3: SDL window has no native drawable");
        return NULL;
    }
    /* The window's inner area, through an offscreen bitmap and ClipBlit
     * (ogles2's window mode; a fullscreen SDL window is a window on its own
     * screen). */
    ctx = aglCreateContextTags2(&err,
                                OGLES2_CCT_WINDOW, (ULONG)iwin,
                                OGLES2_CCT_VSYNC, (ULONG)os3_gles2_swap_interval,
                                OGLES2_CCT_DEPTH, (ULONG)_this->gl_config.depth_size,
                                OGLES2_CCT_STENCIL, (ULONG)_this->gl_config.stencil_size,
                                TAG_DONE);
    if (!ctx) {
        SDL_SetError("OS3: ogles2.library could not create a context (error %lu)", (unsigned long)err);
        return NULL;
    }
    data->gl_context = ctx;
    data->gl_es = 1;
    data->gl_width = iwin->Width - iwin->BorderLeft - iwin->BorderRight;
    data->gl_height = iwin->Height - iwin->BorderTop - iwin->BorderBottom;
    aglMakeCurrent(ctx);
    glViewport(0, 0, data->gl_width, data->gl_height);
    return (SDL_GLContext)ctx;
}

int OS3_GLES2_MakeCurrent(_THIS, SDL_Window *window, SDL_GLContext context)
{
    (void)_this;
    (void)window;
    aglMakeCurrent(context); /* NULL: none */
    return 0;
}

/* The window resized or replaced (fullscreen toggle): the context renders
 * into it at its size (ogles2's aglSetParams2(OGLES2_CCT_WINDOW)); objects
 * and state stay. */
void OS3_GLES2_ResizeWindow(_THIS, SDL_Window *window, int w, int h)
{
    OS3_WindowData *data = window ? (OS3_WindowData *)window->driverdata : NULL;
    void *previous = OGLES2CurrentContext;
    (void)_this;
    if (!data || !data->gl_context || !data->window || w <= 0 || h <= 0) {
        return;
    }
    if (w != data->gl_width || h != data->gl_height) {
        aglMakeCurrent(data->gl_context);
        aglSetParamsTags2(OGLES2_CCT_WINDOW, (ULONG)data->window, TAG_DONE);
        data->gl_width = w;
        data->gl_height = h;
        if (previous != data->gl_context) {
            aglMakeCurrent(previous);
        }
    }
}

void OS3_GLES2_GetDrawableSize(_THIS, SDL_Window *window, int *w, int *h)
{
    OS3_WindowData *data = window ? (OS3_WindowData *)window->driverdata : NULL;
    if (data && data->window) {
        int width = data->window->Width - data->window->BorderLeft - data->window->BorderRight;
        int height = data->window->Height - data->window->BorderTop - data->window->BorderBottom;
        OS3_GLES2_ResizeWindow(_this, window, width, height);
        if (w) *w = width;
        if (h) *h = height;
    } else {
        if (w) *w = window ? window->w : 0;
        if (h) *h = window ? window->h : 0;
    }
}

int OS3_GLES2_SetSwapInterval(_THIS, int interval)
{
    (void)_this;
    if (interval != 0 && interval != 1) {
        return SDL_SetError("OS3: ogles2.library supports swap interval 0 or 1 only");
    }
    os3_gles2_swap_interval = interval;
    if (OGLES2CurrentContext) {
        aglSetParamsTags2(OGLES2_CCT_VSYNC, (ULONG)interval, TAG_DONE);
    }
    return 0;
}

int OS3_GLES2_GetSwapInterval(_THIS)
{
    (void)_this;
    return os3_gles2_swap_interval;
}

int OS3_GLES2_SwapWindow(_THIS, SDL_Window *window)
{
    OS3_WindowData *data;

    if (!window || !window->driverdata) {
        return SDL_SetError("OS3: invalid SDL window for an OpenGL ES 2 swap");
    }
    data = (OS3_WindowData *)window->driverdata;
    if (!data->gl_context) {
        return SDL_SetError("OS3: SDL window has no OpenGL ES 2 context");
    }
    /* Host-owned windows resize outside SDL's IDCMP event loop. */
    if (data->window) {
        int w = data->window->Width - data->window->BorderLeft - data->window->BorderRight;
        int h = data->window->Height - data->window->BorderTop - data->window->BorderBottom;
        OS3_GLES2_ResizeWindow(_this, window, w, h);
    }
    if (OGLES2CurrentContext != data->gl_context) {
        aglMakeCurrent(data->gl_context);
    }
    /* ogles2 renders the frame and presents it (waiting for the vertical
     * blank at swap interval 1); no glFinish, which would stop the next
     * frame's CPU work overlapping this one's render. */
    aglSwapBuffers();
    return 0;
}

void OS3_GLES2_DeleteContext(_THIS, SDL_GLContext context)
{
    SDL_Window *window;
    if (!context) {
        return;
    }
    for (window = _this->windows; window; window = window->next) {
        OS3_WindowData *data = (OS3_WindowData *)window->driverdata;
        if (data && data->gl_context == context) {
            data->gl_context = NULL;
            data->gl_es = 0;
            data->gl_width = data->gl_height = 0;
            break;
        }
    }
    if (OGLES2CurrentContext == context) {
        aglMakeCurrent(NULL);
    }
    aglDestroyContext(context);
}

/* Is this context one of ours (SDL_os3opengl.c's direct calls, from the
 * window and event code, hand ogles2 contexts over)? */
SDL_bool OS3_GLES2_OwnsWindow(SDL_Window *window)
{
    OS3_WindowData *data = window ? (OS3_WindowData *)window->driverdata : NULL;
    return data && data->gl_context && data->gl_es ? SDL_TRUE : SDL_FALSE;
}

SDL_bool OS3_GLES2_OwnsContext(_THIS, SDL_GLContext context)
{
    SDL_Window *window;
    for (window = _this->windows; window; window = window->next) {
        OS3_WindowData *data = (OS3_WindowData *)window->driverdata;
        if (data && data->gl_context == context) {
            return data->gl_es ? SDL_TRUE : SDL_FALSE;
        }
    }
    return SDL_FALSE;
}

#endif /* SDL_VIDEO_DRIVER_AMIGAOS3 && SDL_VIDEO_OPENGL_ES2 */
