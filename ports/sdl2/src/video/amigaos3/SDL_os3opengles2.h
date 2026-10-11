/*
  SDL2 OpenGL ES 2.0 backend -- AmigaOS 3.x ogles2.library (ogles2, ports/sdl2).
  zlib licence, as SDL; see SDL_os3opengles2.c.
*/
#ifndef SDL_os3opengles2_h_
#define SDL_os3opengles2_h_

#include "../../SDL_internal.h"
#include "../SDL_sysvideo.h"

#if SDL_VIDEO_DRIVER_AMIGAOS3 && defined(SDL_VIDEO_OPENGL_ES2)

extern int OS3_GL_LoadLibraryAny(_THIS, const char *path); /* chooses ogles2 or MiniGL */
extern int OS3_GLES2_LoadLibrary(_THIS, const char *path);
extern void OS3_GLES2_UnloadLibrary(_THIS);
extern void *OS3_GLES2_GetProcAddress(_THIS, const char *proc);
extern SDL_GLContext OS3_GLES2_CreateContext(_THIS, SDL_Window *window);
extern int OS3_GLES2_MakeCurrent(_THIS, SDL_Window *window, SDL_GLContext context);
extern void OS3_GLES2_GetDrawableSize(_THIS, SDL_Window *window, int *w, int *h);
extern int OS3_GLES2_SetSwapInterval(_THIS, int interval);
extern int OS3_GLES2_GetSwapInterval(_THIS);
extern int OS3_GLES2_SwapWindow(_THIS, SDL_Window *window);
extern void OS3_GLES2_DeleteContext(_THIS, SDL_GLContext context);
extern void OS3_GLES2_ResizeWindow(_THIS, SDL_Window *window, int w, int h);
extern SDL_bool OS3_GLES2_OwnsWindow(SDL_Window *window);
extern SDL_bool OS3_GLES2_OwnsContext(_THIS, SDL_GLContext context);

#endif
#endif
