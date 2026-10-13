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
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
/*
 * ioquake3's GLES2 renderer (renderergl2) for AmigaOS 3 on ogles2.library
 * directly (ports/ioq3-gl2): the GL window system part, in place of
 * upstream's code/sdl/sdl_glimp.c. The context as ports/ioq3/amiga_glimp.c
 * makes it (ogles2's own screen in a BGRA32/RGBA32 mode with r_fullscreen 1,
 * its own window with 0; depth 24, stencil 8); the GL functions are those
 * of the glue ports/gl4es/gen_glue.py generates from ogles2.library's
 * dispatch table (each calls the table with the current context; with the
 * environment variable QUAKE3_GL_PROFILE set, each call is counted and
 * timed, and the split is printed at shutdown). Also what Cowcat's Amiga engine code takes
 * from the glimp it was written for (the window, its message port, the
 * mouse pointer calls).
 */

#include "../renderergl2/tr_local.h"

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
#include <libraries/ogles2.h>

/* libogles2.a (<proto/ogles2.h> is not included: its GL wrappers would
 * clash with the desktop GL declarations qgl.h brings). */
void *aglCreateContextTags2(ULONG *errcode, ...);
void aglSetParamsTags2(ULONG tag1, ...);
void aglDestroyContext(void *context);
void aglMakeCurrent(void *context);
void aglSwapBuffers(void);

/* The generated glue (ogles2_glue.c, ports/gl4es/gen_glue.py). */
void os4OpenLib(void **lib);
void os4CloseLib(void);
void *os4GetProcAddress(const char *name);
void gl4es_glue_profile(ULONG frames);
static ULONG frames;

/* The Amiga engine's (code/amiga). */
void IN_Init(void);
void IN_Shutdown(void);

struct IntuitionBase *IntuitionBase = NULL;
struct Library *CyberGfxBase = NULL;
struct GfxBase *GfxBase = NULL;
static void *glcontext;
static qboolean lib_open;

struct MsgPort *Sys_EventPort = 0;
struct Window *win = NULL;
qboolean windowmode; /* amiga_input.c: IN_Frame */

int qglMajorVersion, qglMinorVersion;
int qglesMajorVersion, qglesMinorVersion;

void (APIENTRYP qglActiveTextureARB) (GLenum texture);
void (APIENTRYP qglClientActiveTextureARB) (GLenum texture);
void (APIENTRYP qglMultiTexCoord2fARB) (GLenum target, GLfloat s, GLfloat t);
void (APIENTRYP qglLockArraysEXT) (GLint first, GLsizei count);
void (APIENTRYP qglUnlockArraysEXT) (void);

#define GLE(ret, name, ...) name##proc * qgl##name = NULL;
QGL_1_1_PROCS;
QGL_1_1_FIXED_FUNCTION_PROCS;
QGL_DESKTOP_1_1_PROCS;
QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
QGL_ES_1_1_PROCS;
QGL_ES_1_1_FIXED_FUNCTION_PROCS;
QGL_1_3_PROCS;
QGL_1_5_PROCS;
QGL_2_0_PROCS;
QGL_3_0_PROCS;
QGL_ARB_occlusion_query_PROCS;
QGL_ARB_framebuffer_object_PROCS;
QGL_ARB_vertex_array_object_PROCS;
QGL_EXT_direct_state_access_PROCS;
#undef GLE

/* tr_extensions.c's SDL_GL_GetProcAddress and SDL_GL_ExtensionSupported
 * (ports/ioq3-gl2/include/SDL.h). */
void *GLimp_ProcAddress(const char *name)
{
	return os4GetProcAddress(name);
}

int GLimp_HaveExtension(const char *name)
{
	const char *e = glConfig.extensions_string;
	size_t n = strlen(name);
	while ((e = strstr(e, name)) != NULL)
	{
		if ((e == glConfig.extensions_string || e[-1] == ' ') && (e[n] == ' ' || e[n] == 0))
			return 1;
		e += n;
	}
	return 0;
}

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

/* GLES 2.0 has no double-precision or desktop-only calls (sdl_glimp.c's). */
static void APIENTRY GLimp_GLES_ClearDepth(GLclampd depth)
{
	qglClearDepthf(depth);
}

static void APIENTRY GLimp_GLES_DepthRange(GLclampd near_val, GLclampd far_val)
{
	qglDepthRangef(near_val, far_val);
}

static void APIENTRY GLimp_GLES_DrawBuffer(GLenum mode)
{
}

static void APIENTRY GLimp_GLES_PolygonMode(GLenum face, GLenum mode)
{
}

/* The OpenGL ES 2.0 set, as sdl_glimp.c's GLimp_GetProcAddresses takes it. */
static qboolean GLimp_GetProcAddresses(void)
{
	qboolean success = qtrue;
	const char *version;
	char profile[6];

#define GLE(ret, name, ...) qgl##name = (name##proc *)os4GetProcAddress("gl" #name); \
	if (qgl##name == NULL) { \
		ri.Printf(PRINT_ALL, "ERROR: Missing OpenGL function %s\n", "gl" #name); \
		success = qfalse; \
	}
	GLE(const GLubyte *, GetString, GLenum name)
	if (!qglGetString)
		return qfalse;
	version = (const char *)qglGetString(GL_VERSION);
	if (!version || Q_stricmpn("OpenGL ES", version, 9) != 0)
	{
		ri.Printf(PRINT_ALL, "GL_VERSION \"%s\": not OpenGL ES\n", version ? version : "");
		return qfalse;
	}
	sscanf(version, "OpenGL %5s %d.%d", profile, &qglesMajorVersion, &qglesMinorVersion);
	qglMajorVersion = qglMinorVersion = 0;
	if (!QGLES_VERSION_ATLEAST(2, 0))
		return qfalse;
	QGL_1_1_PROCS;
	QGL_ES_1_1_PROCS;
	QGL_1_3_PROCS;
	QGL_1_5_PROCS;
	QGL_2_0_PROCS;
#undef GLE
	qglClearDepth = GLimp_GLES_ClearDepth;
	qglDepthRange = GLimp_GLES_DepthRange;
	qglDrawBuffer = GLimp_GLES_DrawBuffer;
	qglPolygonMode = GLimp_GLES_PolygonMode;
	return success;
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

/* Compiling the renderer's shaders on the Amiga takes about 90 s the first
 * time a screen mode is used (later starts load them from OGLES2Cache: in
 * about 1.4 s), and nothing is drawn meanwhile. GLSL_InitGPUShader calls
 * this after each program (ports/ioq3-gl2/patches/0002): once a program has
 * taken longer than a cache load could, a line and a progress bar are drawn
 * straight into the window's RastPort (the screen's bitmap the GPU renders
 * into; the first frame covers them). No GL state is touched. */
#define SHADERS_EXPECTED 109 /* the default settings' programs */
static int shader_count, shader_last, shader_shown;

void GLimp_ShaderProgress(void)
{
	int now = ri.Milliseconds(), slow = shader_count > 0 && now - shader_last > 150;
	struct RastPort *rp;
	char line[128];
	int x0, y0, w, h, bw, bh, bx, by, fill, len, tw;

	shader_count++;
	shader_last = now;
	if ((!shader_shown && !slow) || !win || !CyberGfxBase)
		return;
	rp = win->RPort;
	x0 = win->BorderLeft;
	y0 = win->BorderTop;
	w = win->Width - win->BorderLeft - win->BorderRight;
	h = win->Height - win->BorderTop - win->BorderBottom;
	if (w < 64 || h < 64)
		return;
	if (!shader_shown)
	{
		FillPixelArray(rp, x0, y0, w, h, 0x00000000);
		shader_shown = 1;
	}
	Com_sprintf(line, sizeof(line), "Compiling shaders for this screen mode, once: %d of about %d",
	            shader_count, SHADERS_EXPECTED);
	len = strlen(line);
	tw = TextLength(rp, line, len);
	bw = w / 2;
	bh = rp->TxHeight + 4;
	bx = x0 + (w - bw) / 2;
	by = y0 + h / 2;
	FillPixelArray(rp, x0, by - 3 * bh, w, 2 * bh, 0x00000000); /* the text's line */
	SetAPen(rp, 2);
	SetDrMd(rp, JAM1);
	Move(rp, x0 + (w - tw) / 2, by - 2 * bh + rp->TxBaseline);
	Text(rp, line, len);
	fill = bw * (shader_count < SHADERS_EXPECTED ? shader_count : SHADERS_EXPECTED) / SHADERS_EXPECTED;
	FillPixelArray(rp, bx, by, bw, bh, 0x00303030);
	if (fill > 0)
		FillPixelArray(rp, bx, by, fill, bh, 0x00c08020);
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

static qboolean GLimp_SetMode(int mode, qboolean fullscreen)
{
	ULONG err = 0, w = 0, h = 0;

	ri.Printf(PRINT_ALL, "...setting mode %d:", mode);
	if (!R_GetModeInfo(&glConfig.vidWidth, &glConfig.vidHeight, &glConfig.windowAspect, mode))
	{
		ri.Printf(PRINT_ALL, " invalid mode\n");
		return qfalse;
	}
	ri.Printf(PRINT_ALL, " %d %d %s\n", glConfig.vidWidth, glConfig.vidHeight, fullscreen ? "fullscreen" : "windowed");

	if (fullscreen)
	{
		ULONG id = find_mode(glConfig.vidWidth, glConfig.vidHeight);
		if (id == (ULONG)INVALID_ID)
		{
			ri.Printf(PRINT_ALL, "...no BGRA32/RGBA32 screen mode of %dx%d\n", glConfig.vidWidth, glConfig.vidHeight);
			return qfalse;
		}
		/* r_swapInterval 1 (latched, as upstream's): ogles2 flips two screen
		 * buffers at the display's refresh, no tearing (plan 7.2). */
		glcontext = aglCreateContextTags2(&err, OGLES2_CCT_MODEID, id, OGLES2_CCT_PISTORM_WIDTH, glConfig.vidWidth,
		                                  OGLES2_CCT_PISTORM_HEIGHT, glConfig.vidHeight, OGLES2_CCT_DEPTH, 24,
		                                  OGLES2_CCT_STENCIL, 8, OGLES2_CCT_VSYNC,
		                                  (ULONG)(r_swapInterval->integer ? TRUE : FALSE), TAG_DONE);
	}
	else
	{
		glcontext = aglCreateContextTags2(&err, OGLES2_CCT_PISTORM_WIDTH, glConfig.vidWidth,
		                                  OGLES2_CCT_PISTORM_HEIGHT, glConfig.vidHeight, OGLES2_CCT_DEPTH, 24,
		                                  OGLES2_CCT_STENCIL, 8, OGLES2_CCT_VSYNC, FALSE, TAG_DONE);
	}
	if (!glcontext)
	{
		ri.Printf(PRINT_ALL, "...aglCreateContext2 failed (%lu)\n", (unsigned long)err);
		return qfalse;
	}
	aglMakeCurrent(glcontext);
	aglSetParamsTags2(OGLES2_CCT_GET_WIDTH, (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h,
	                  OGLES2_CCT_PISTORM_GET_WINDOW, (ULONG)&win, TAG_DONE);
	ri.Printf(PRINT_ALL, "...ogles2 context %lux%lu\n", (unsigned long)w, (unsigned long)h);
	windowmode = fullscreen ? qfalse : qtrue;

	glConfig.colorBits = 32;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 8;
	glConfig.isFullscreen = fullscreen;
	glConfig.stereoEnabled = qfalse;
	glConfig.displayFrequency = 60;
	displayAspect = glConfig.windowAspect;
	return qtrue;
}

static void GLimp_CloseLibraries(void)
{
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
}

void GLimp_Init(qboolean fixedFunction)
{
	shader_count = shader_shown = 0;
	ri.Printf(PRINT_ALL, "===== quake3-68k, renderergl2 on ogles2.library =====\n");
	if (fixedFunction)
		ri.Error(ERR_FATAL, "GLimp_Init: this build has OpenGL ES 2.0 only (renderergl2)\n");

	if (!IntuitionBase)
		IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
	if (!GfxBase)
		GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
	if (!CyberGfxBase)
		CyberGfxBase = OpenLibrary("cybergraphics.library", 41);
	if (!IntuitionBase || !GfxBase || !CyberGfxBase)
		ri.Error(ERR_FATAL, "GLimp_Init: needs intuition and graphics 39, cybergraphics 41\n");
	if (!lib_open)
	{
		void *lib = NULL;
		os4OpenLib(&lib);
		if (!lib)
			ri.Error(ERR_FATAL, "GLimp_Init: ogles2.library (1.1 or newer) did not open\n");
		lib_open = qtrue;
		frames = 0;
	}

	if (!GLimp_SetMode(r_mode->integer, r_fullscreen->integer) &&
	    (r_mode->integer == 3 || !GLimp_SetMode(3, r_fullscreen->integer)))
		ri.Error(ERR_FATAL, "GLimp_Init: no OpenGL ES context (ogles2.library)\n");
	if (!GLimp_GetProcAddresses())
		ri.Error(ERR_FATAL, "GLimp_Init: ogles2.library lacks a GLES 2.0 function\n");

	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
	glConfig.deviceSupportsGamma = qfalse;
	Q_strncpyz(glConfig.vendor_string, (char *)qglGetString(GL_VENDOR), sizeof(glConfig.vendor_string));
	Q_strncpyz(glConfig.renderer_string, (char *)qglGetString(GL_RENDERER), sizeof(glConfig.renderer_string));
	Q_strncpyz(glConfig.version_string, (char *)qglGetString(GL_VERSION), sizeof(glConfig.version_string));
	Q_strncpyz(glConfig.extensions_string, (char *)qglGetString(GL_EXTENSIONS), sizeof(glConfig.extensions_string));

	/* sdl_glimp.c's GLimp_InitExtensions for a programmable context: no S3TC,
	 * no anisotropy; clamp to edge is core in ES. */
	glConfig.textureCompression = TC_NONE;
	glConfig.textureEnvAddAvailable = qfalse;
	textureFilterAnisotropic = qfalse;
	maxAnisotropy = 0;
	haveClampToEdge = qtrue;

	ModifyIDCMP(win, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_DELTAMOVE);
	win->Flags |= WFLG_REPORTMOUSE;
	Sys_EventPort = win->UserPort;

	qglClearColor(0, 0, 0, 1);
	qglClear(GL_COLOR_BUFFER_BIT);

	GLimp_DetectAvailableModes();
	ri.Cvar_Get("r_availableModes", "", CVAR_ROM);

	IN_Init();
	ri.Printf(PRINT_ALL, "... Sys_EventPort at %p\n", Sys_EventPort);
}

void GLimp_Shutdown(void)
{
	gl4es_glue_profile(frames); /* QUAKE3_GL_PROFILE: the GL calls per frame since the context began */
	ri.Printf(PRINT_ALL, "GLimp_Shutdown: input\n");
	IN_Shutdown();
	ri.Printf(PRINT_ALL, "GLimp_Shutdown: the context\n");
	if (glcontext)
	{
		aglDestroyContext(glcontext);
		glcontext = NULL;
	}
	win = NULL;
	Sys_EventPort = NULL;
	if (empty_pointer)
	{
		FreeVec(empty_pointer);
		empty_pointer = NULL;
	}
	if (lib_open)
	{
		os4CloseLib();
		lib_open = qfalse;
	}
	GLimp_CloseLibraries();
	ri.Printf(PRINT_ALL, "GLimp_Shutdown: done\n");
}

void GLimp_EndFrame(void)
{
	aglSwapBuffers();
	frames++;
}

void GLimp_Minimize(void)
{
}

void GLimp_LogComment(char *comment)
{
}

void GLimp_SetGamma(unsigned char red[256], unsigned char green[256], unsigned char blue[256])
{
}
