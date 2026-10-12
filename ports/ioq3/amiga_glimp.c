/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
/*
 * ioquake3 for AmigaOS 3: the GL window system part on gl4es and
 * ogles2.library (ports/ioq3), in place of code/amiga/amiga_glimp.c, which
 * drives MiniGL. Same structure and cvars as that file (Cowcat's port and
 * its MiniGL library build); the context comes from aglCreateContext2:
 * ogles2's own screen in a BGRA32/RGBA32 mode (r_fullscreen 1) or its own
 * window (0), depth 24 with stencil 8, and the window that takes the input
 * from OGLES2_CCT_PISTORM_GET_WINDOW.
 */

#include "../renderer/tr_local.h"
#include "../client/client.h"
#include "amiga_local.h"

#include <exec/exec.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>

#include <mgl/gl.h> /* ports/ioq3: gl4es's GL */
#include <gl4es_agl.h>

struct IntuitionBase *IntuitionBase = NULL;
struct Library *CyberGfxBase = NULL;
struct GfxBase *GfxBase = NULL; /* the MiniGL driver's, in the other build */
static void *glcontext;

extern cvar_t *r_finish;
cvar_t *r_closeworkbench;
cvar_t *r_guardband;
cvar_t *r_vertexbuffersize;
cvar_t *r_glbuffers;
cvar_t *r_perspective_fast;
cvar_t *r_mintriarea;

extern cvar_t *in_nograb;

struct MsgPort *Sys_EventPort = 0;
struct Window *win = NULL;

void (APIENTRYP qglActiveTextureARB) (GLenum texture);
void (APIENTRYP qglClientActiveTextureARB) (GLenum texture);
void (APIENTRYP qglMultiTexCoord2fARB) (GLenum target, GLfloat s, GLfloat t);

void (APIENTRYP qglLockArraysEXT) (GLint first, GLsizei count);
void (APIENTRYP qglUnlockArraysEXT) (void);

void GLimp_RenderThreadWrapper( void *stub ) {}
void GLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] ) {}
void GLW_InitGamma(void) {}
void GLW_RestoreGamma(void) {}

extern qboolean mouse_avail;
extern qboolean mhandler;
qboolean windowmode; // IN_Frame check

/* MiniGL's mouse pointer calls, as amiga_input.c uses them: mglSetPointer
 * when the game takes the mouse (an empty pointer hides it), mglClearPointer
 * when it gives it back (the console, shutdown): the normal pointer. */
static UWORD *empty_pointer;

void mglSetPointer(void)
{
	if (!win)
		return;
	if (!empty_pointer)
		empty_pointer = AllocVec(16, MEMF_CHIP | MEMF_CLEAR);
	if (empty_pointer)
		SetPointer(win, empty_pointer, 1, 1, 0, 0);
}

void mglClearPointer(void)
{
	if (win)
		ClearPointer(win);
}

/* A 32-bit RTG mode of w x h the GPU can store into (BGRA32 or RGBA32). */
static ULONG find_mode(int w, int h)
{
	ULONG id = INVALID_ID, best = INVALID_ID;
	while ((id = NextDisplayInfo(id)) != (ULONG)INVALID_ID)
	{
		ULONG f;
		if (!IsCyberModeID(id))
			continue;
		f = GetCyberIDAttr(CYBRIDATTR_PIXFMT, id);
		if (f != PIXFMT_BGRA32 && f != PIXFMT_RGBA32)
			continue;
		if (GetCyberIDAttr(CYBRIDATTR_WIDTH, id) == (ULONG)w && GetCyberIDAttr(CYBRIDATTR_HEIGHT, id) == (ULONG)h)
			return id;
		if (best == (ULONG)INVALID_ID && GetCyberIDAttr(CYBRIDATTR_WIDTH, id) >= (ULONG)w &&
		    GetCyberIDAttr(CYBRIDATTR_HEIGHT, id) >= (ULONG)h)
			best = id;
	}
	return best;
}

/* r_availableModes for the video menu (q3_ui's GraphicsOptions_GetResolutions,
 * as sdl_glimp.c's GLimp_DetectAvailableModes): every size of a 32-bit RTG
 * mode the GPU can store into, smallest first, "WxH WxH ...". A size not in
 * the menu's built-in list is set through r_mode -1 and r_customwidth/height,
 * which find_mode opens. */
static void GLimp_DetectAvailableModes(void)
{
	enum { MAX_SIZES = 31 }; /* q3_ui keeps 32 entries, the last a NULL */
	ULONG w[MAX_SIZES], h[MAX_SIZES], id = INVALID_ID;
	char buf[MAX_STRING_CHARS] = "";
	int n = 0, i, j;

	while ((id = NextDisplayInfo(id)) != (ULONG)INVALID_ID)
	{
		ULONG f, mw, mh;
		if (!IsCyberModeID(id))
			continue;
		f = GetCyberIDAttr(CYBRIDATTR_PIXFMT, id);
		if (f != PIXFMT_BGRA32 && f != PIXFMT_RGBA32)
			continue;
		mw = GetCyberIDAttr(CYBRIDATTR_WIDTH, id);
		mh = GetCyberIDAttr(CYBRIDATTR_HEIGHT, id);
		if (mw < 640 || mh < 480) /* the many small modes would crowd out the large ones */
			continue;
		for (i = 0; i < n && (w[i] != mw || h[i] != mh); i++)
			;
		if (i < n || n == MAX_SIZES)
			continue;
		/* insertion by area, then width */
		for (i = n; i > 0 && (w[i - 1] * h[i - 1] > mw * mh || (w[i - 1] * h[i - 1] == mw * mh && w[i - 1] > mw)); i--)
		{
			w[i] = w[i - 1];
			h[i] = h[i - 1];
		}
		w[i] = mw;
		h[i] = mh;
		n++;
	}
	for (j = 0; j < n; j++)
		Q_strcat(buf, sizeof(buf), va("%s%lux%lu", j ? " " : "", (unsigned long)w[j], (unsigned long)h[j]));
	if (*buf)
	{
		ri.Printf(PRINT_ALL, "Available modes: '%s'\n", buf);
		ri.Cvar_Set("r_availableModes", buf);
	}
}

static qboolean GLW_StartDriverAndSetMode( int mode, int colorbits, qboolean fullscreen )
{
	ULONG err = 0, w = 0, h = 0;

	ri.Printf(PRINT_ALL, "...setting mode %d:", mode);

	if (!R_GetModeInfo( &glConfig.vidWidth, &glConfig.vidHeight, &glConfig.windowAspect, mode))
	{
		ri.Printf(PRINT_ALL, " invalid mode\n");
		return qfalse;
	}
	ri.Printf( PRINT_ALL, " %d %d %s\n", glConfig.vidWidth, glConfig.vidHeight, fullscreen ? "fullscreen" : "windowed" );

	/* MiniGL's cvars, kept so a shared q3config.cfg means the same; unused here. */
	r_vertexbuffersize  = Cvar_Get("r_vertexbuffersize", "4096", CVAR_ARCHIVE | CVAR_LATCH);
	r_glbuffers  = Cvar_Get("r_glbuffers", "3", CVAR_ARCHIVE);
	r_guardband  = Cvar_Get("r_guardband", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_closeworkbench = Cvar_Get("r_closeworkbench", "0", CVAR_ARCHIVE);
	r_perspective_fast = Cvar_Get("r_perspective_fast", "0", CVAR_ARCHIVE | CVAR_LATCH);

	if (fullscreen)
	{
		ULONG id = find_mode(glConfig.vidWidth, glConfig.vidHeight);
		if (id == (ULONG)INVALID_ID)
		{
			ri.Printf(PRINT_ALL, "...no BGRA32/RGBA32 screen mode of %dx%d\n", glConfig.vidWidth, glConfig.vidHeight);
			return qfalse;
		}
		glcontext = aglCreateContextTags2(&err, OGLES2_CCT_MODEID, id, OGLES2_CCT_PISTORM_WIDTH, glConfig.vidWidth,
		                                  OGLES2_CCT_PISTORM_HEIGHT, glConfig.vidHeight, OGLES2_CCT_DEPTH, 24,
		                                  OGLES2_CCT_STENCIL, 8, OGLES2_CCT_VSYNC, FALSE, TAG_DONE);
	}
	else
	{
		glcontext = aglCreateContextTags2(&err, OGLES2_CCT_PISTORM_WIDTH, glConfig.vidWidth,
		                                  OGLES2_CCT_PISTORM_HEIGHT, glConfig.vidHeight, OGLES2_CCT_DEPTH, 24,
		                                  OGLES2_CCT_STENCIL, 8, OGLES2_CCT_VSYNC, r_finish->integer ? TRUE : FALSE,
		                                  TAG_DONE);
	}
	if (!glcontext)
	{
		ri.Printf(PRINT_ALL, "...aglCreateContext2 failed (%lu)\n", (unsigned long)err);
		return qfalse;
	}
	aglMakeCurrent(glcontext);
	aglSetParamsTags2(OGLES2_CCT_GET_WIDTH, (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h,
	                  OGLES2_CCT_PISTORM_GET_WINDOW, (ULONG)&win, TAG_DONE);
	ri.Printf(PRINT_ALL, "...ogles2 context %lux%lu through gl4es\n", (unsigned long)w, (unsigned long)h);
	windowmode = fullscreen ? qfalse : qtrue;

	glConfig.colorBits = 32;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 8;
	glConfig.isFullscreen = fullscreen;
	return qtrue;
}

static void GLW_Shutdown(void)
{
	if (glcontext)
	{
		aglDestroyContext(glcontext);
		glcontext = NULL;
	}
	win = NULL;
	if (empty_pointer)
	{
		FreeVec(empty_pointer);
		empty_pointer = NULL;
	}
	if (CyberGfxBase)
	{
		CloseLibrary(CyberGfxBase);
		CyberGfxBase = NULL;
	}
	if (GfxBase)
	{
		CloseLibrary((struct Library *)GfxBase);
		GfxBase = NULL;
	}
	if (IntuitionBase)
	{
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
	}
	Sys_EventPort = NULL;
}

static qboolean GLW_LoadOpenGL(void)
{
	qboolean fullscreen;

	if (!IntuitionBase)
		IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
	if (!GfxBase)
		GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
	if (!CyberGfxBase)
		CyberGfxBase = OpenLibrary("cybergraphics.library", 41);
	if (!IntuitionBase || !GfxBase || !CyberGfxBase)
	{
		ri.Printf(PRINT_ALL, "GLW_LoadOpenGL: needs intuition and graphics 39, cybergraphics 41\n");
		return qfalse;
	}
	fullscreen = r_fullscreen->integer;
	if (!GLW_StartDriverAndSetMode(r_mode->integer, r_colorbits->integer, fullscreen))
	{
		if (r_mode->integer != 3 && GLW_StartDriverAndSetMode(3, r_colorbits->integer, fullscreen))
			return qtrue;
		return qfalse;
	}
	return qtrue;
}

void GLW_StartOpenGL(void)
{
	GLW_LoadOpenGL();
}

static void stub_glMultiTexCoord2fARB(GLenum target, GLfloat s, GLfloat t)
{
	glMultiTexCoord2fARB(target, s, t);
}

static void stub_glActiveTextureARB(GLenum texture)
{
	glActiveTextureARB(texture);
}

static void stub_glClientActiveTextureARB(GLenum texture)
{
	glClientActiveTextureARB(texture);
}

static void stub_glLockArraysEXT(GLint first, GLsizei count)
{
	glLockArraysEXT(first, count);
}

static void stub_glUnlockArraysEXT(void)
{
	glUnlockArraysEXT();
}

static void GLW_InitExtensions( void )
{
	if ( !r_allowExtensions->integer )
	{
		ri.Printf( PRINT_ALL, "*** IGNORING OPENGL EXTENSIONS ***\n" );
		return;
	}
	ri.Printf( PRINT_ALL, "Initializing OpenGL extensions\n" );

	/* As the MiniGL build: no S3TC, no texture_env_add (tr_shader.c's
	 * collapse table then keeps every stage). */
	glConfig.textureCompression = TC_NONE;
	ri.Printf( PRINT_ALL, "...GL_S3_s3tc not used\n" );
	glConfig.textureEnvAddAvailable = qfalse;
	ri.Printf( PRINT_ALL, "...GL_EXT_texture_env_add not used\n" );

	if ( strstr( glConfig.extensions_string, "GL_ARB_multitexture" ) && r_ext_multitexture->integer )
	{
		GLint glint = 0;
		qglGetIntegerv( GL_MAX_TEXTURE_UNITS_ARB, &glint );
		glConfig.numTextureUnits = (int) glint;
		if ( glConfig.numTextureUnits > 1 )
		{
			qglMultiTexCoord2fARB = stub_glMultiTexCoord2fARB;
			qglActiveTextureARB = stub_glActiveTextureARB;
			qglClientActiveTextureARB = stub_glClientActiveTextureARB;
			ri.Printf( PRINT_ALL, "...using GL_ARB_multitexture\n" );
		}
		else
		{
			qglMultiTexCoord2fARB = NULL;
			qglActiveTextureARB = NULL;
			qglClientActiveTextureARB = NULL;
			ri.Printf( PRINT_ALL, "...not using GL_ARB_multitexture, < 2 texture units\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...not using GL_ARB_multitexture\n" );
	}

	qglLockArraysEXT = stub_glLockArraysEXT;
	qglUnlockArraysEXT = stub_glUnlockArraysEXT;
	ri.Printf( PRINT_ALL, "...using GL_EXT_compiled_vertex_array\n" );
}

void GLimp_Init(void)
{
	char buf[1024];

	ri.Printf(PRINT_ALL, "===== quake3-68k on gl4es and ogles2.library =====\n");
	ri.Printf(PRINT_ALL, "Initializing OpenGL subsystem\n");

	GLW_StartOpenGL();
	if (!glcontext)
		ri.Error(ERR_FATAL, "GLimp_Init: no OpenGL context (ogles2.library and gl4es)\n");

	Q_strncpyz( glConfig.vendor_string, (char *)qglGetString (GL_VENDOR), sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, (char *)qglGetString (GL_RENDERER), sizeof( glConfig.renderer_string ) );
	Q_strncpyz( glConfig.version_string, (char *)qglGetString (GL_VERSION), sizeof( glConfig.version_string ) );
	Q_strncpyz( glConfig.extensions_string, (char *)qglGetString (GL_EXTENSIONS), sizeof( glConfig.extensions_string ) );

	Q_strncpyz( buf, glConfig.renderer_string, sizeof(buf) );
	Q_strlwr( buf );
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;

	GLW_InitExtensions();
	GLW_InitGamma();

	ModifyIDCMP(win, IDCMP_RAWKEY|IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_DELTAMOVE);
	win->Flags |= WFLG_REPORTMOUSE;
	Sys_EventPort = win->UserPort;

	qglClearColor(0,0,0,1);
	qglClear(GL_COLOR_BUFFER_BIT);

	GLimp_DetectAvailableModes();
	ri.Cvar_Get("r_availableModes", "", CVAR_ROM);

	IN_Init(); // was in amiga_main/sys_init ---

	ri.Printf(PRINT_ALL, "... Sys_EventPort at %p\n", Sys_EventPort);
}

/* The renderer's GL time: draws (gl4es and ogles2.library), array locks,
 * glEnd, and the swap, per frame (the Pi's 1 MHz system timer). */
static ULONG prof_frames, prof_draws, prof_draw_us, prof_lock_us, prof_end_us, prof_swap_us;
unsigned long ioq3_state_us, ioq3_state_calls, ioq3_t0; /* <mgl/gl.h>'s IOQ3_TIMED */
unsigned long ioq3_fn_us[32], ioq3_fn_calls[32];

static ULONG prof_now(void)
{
	ULONG v = *(volatile ULONG *)(0xF2000000 + 0x3004);
	return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

static void prof_report(void)
{
	ULONG n = prof_frames ? prof_frames : 1;
	ri.Printf(PRINT_ALL, "gl profile: %lu frames; per frame %lu draws taking %lu us (gl4es and ogles2), "
	          "%lu us locking arrays, %lu us in glEnd, %lu us swapping, %lu state calls taking %lu us\n",
	          prof_frames, prof_draws / n, prof_draw_us / n, prof_lock_us / n, prof_end_us / n,
	          prof_swap_us / n, ioq3_state_calls / n, ioq3_state_us / n);
	{
		/* By function, the names from <mgl/gl.h> (space-separated, in index order). */
		const char *names = IOQ3_TIMED_NAMES;
		int k;
		for (k = 0; k < 32 && *names; k++) {
			char name[40];
			int l = 0;
			while (*names && *names != ' ' && l < 39)
				name[l++] = *names++;
			name[l] = 0;
			while (*names == ' ')
				names++;
			if (ioq3_fn_calls[k])
				ri.Printf(PRINT_ALL, "gl profile: %-28s %6lu calls %6lu us a frame\n", name,
				          ioq3_fn_calls[k] / n, ioq3_fn_us[k] / n);
		}
	}
}

void GLimp_Shutdown(void)
{
	prof_report();
	ri.Printf(PRINT_ALL, "GLimp_Shutdown: input\n");
	IN_Shutdown();
	GLW_RestoreGamma();
	ri.Printf(PRINT_ALL, "GLimp_Shutdown: the context\n");
	GLW_Shutdown();
	ri.Printf(PRINT_ALL, "GLimp_Shutdown: done\n");
}

void GLimp_EndFrame(void)
{
	if (r_finish->modified)
	{
		aglSetParamsTags2(OGLES2_CCT_VSYNC, r_finish->integer ? TRUE : FALSE, TAG_DONE);
		r_finish->modified = qfalse;
	}
	{
		ULONG t0 = prof_now();
		aglSwapBuffers();
		prof_swap_us += prof_now() - t0;
		prof_frames++;
	}
}

#undef glDrawElements
#undef glDrawArrays
#undef glEnd
#undef glLockArraysEXT
#undef glUnlockArraysEXT

void ioq3_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	ULONG t0 = prof_now();
	glDrawElements(mode, count, type, indices);
	prof_draw_us += prof_now() - t0;
	prof_draws++;
}

void ioq3_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	ULONG t0 = prof_now();
	glDrawArrays(mode, first, count);
	prof_draw_us += prof_now() - t0;
	prof_draws++;
}

void ioq3_glEnd(void)
{
	ULONG t0 = prof_now();
	glEnd();
	prof_end_us += prof_now() - t0;
}

void ioq3_glLockArraysEXT(GLint first, GLsizei count)
{
	ULONG t0 = prof_now();
	glLockArraysEXT(first, count);
	prof_lock_us += prof_now() - t0;
}

void ioq3_glUnlockArraysEXT(void)
{
	ULONG t0 = prof_now();
	glUnlockArraysEXT();
	prof_lock_us += prof_now() - t0;
}
