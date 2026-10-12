/*
 * ports/ioq3-gl2: the two SDL calls renderergl2's tr_extensions.c makes,
 * answered by amiga_glimp_gl2.c (a name lookup in the generated qgl table
 * and a search of GL_EXTENSIONS).
 */
#ifndef IOQ3_GL2_SDL_H
#define IOQ3_GL2_SDL_H
void *GLimp_ProcAddress(const char *name);
int GLimp_HaveExtension(const char *name);
#define SDL_GL_GetProcAddress(name) GLimp_ProcAddress(name)
#define SDL_GL_ExtensionSupported(name) GLimp_HaveExtension(name)
#endif
