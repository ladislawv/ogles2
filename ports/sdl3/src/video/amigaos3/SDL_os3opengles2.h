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

/* ALTERED SOURCE (ogles2, ports/sdl3): see SDL_os3opengles2.c. */
#ifndef SDL_os3opengles2_h_
#define SDL_os3opengles2_h_

#include "SDL_os3video.h"

bool OS3_GL_LoadLibraryAny(SDL_VideoDevice *_this, const char *path);

bool OS3_GLES2_LoadLibrary(SDL_VideoDevice *_this, const char *path);
void OS3_GLES2_UnloadLibrary(SDL_VideoDevice *_this);
SDL_FunctionPointer OS3_GLES2_GetProcAddress(SDL_VideoDevice *_this, const char *proc);
SDL_GLContext OS3_GLES2_CreateContext(SDL_VideoDevice *_this, SDL_Window *window);
bool OS3_GLES2_MakeCurrent(SDL_VideoDevice *_this, SDL_Window *window, SDL_GLContext context);
bool OS3_GLES2_SetSwapInterval(SDL_VideoDevice *_this, int interval);
bool OS3_GLES2_GetSwapInterval(SDL_VideoDevice *_this, int *interval);
bool OS3_GLES2_SwapWindow(SDL_VideoDevice *_this, SDL_Window *window);
bool OS3_GLES2_DestroyContext(SDL_VideoDevice *_this, SDL_GLContext context);
void OS3_GLES2_ResizeWindow(SDL_VideoDevice *_this, SDL_Window *window, int w, int h);

bool OS3_GLES2_OwnsWindow(SDL_Window *window);
bool OS3_GLES2_OwnsContext(SDL_VideoDevice *_this, SDL_GLContext context);

#endif /* SDL_os3opengles2_h_ */
