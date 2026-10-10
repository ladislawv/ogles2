/*
 * Independent reference (x86-64): Mesa's own EGL surfaceless + GLES2 on the
 * v3d driver under Mesa's drm-shim (V3D_GPU_ID=42), not the fake device.
 * compare_v3d.sh checks v3dglslc's QPU listings against it.
 *   usage: s1ref vs.vert fs.frag [vs2 fs2 ...]
 *   run with LD_PRELOAD=libv3d_noop_drm_shim.so V3D_GPU_ID=42
 *            MESA_LOADER_DRIVER_OVERRIDE=v3d V3D_DEBUG=precompile,qpu
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include "compile_common.c"

int main(int argc, char **argv)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC getdpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getdpy(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint major, minor, n;
    EGLConfig cfg;
    EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx;

    if (!eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed 0x%x\n", eglGetError());
        return 20;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "no EGL config\n");
        return 20;
    }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    /* Surfaceless, like the m68k side: no framebuffer. */
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "eglMakeCurrent failed 0x%x\n", eglGetError());
        return 20;
    }
    fprintf(stderr, "v3dglslc: renderer %s\n", (const char *)glGetString(GL_RENDERER));
    return run_pairs(argc, argv, 1);
}
