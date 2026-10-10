/*
 * corpus.h's backend on the PC: Mesa's llvmpipe through surfaceless EGL,
 * drawing into a framebuffer object that stands in for the window (the
 * reference images). It also records each linked program's exact shader
 * sources (host_hooks.h routes the programs' glShaderSource and
 * glLinkProgram here) in CORPUS_SHADERS (default corpus_shaders.txt), in
 * the format of ogles2's T:ogles2_missing_shaders.txt, for the offline
 * compiler.
 */
/* host_hooks.h is force-included here too: this file calls the real ones. */
#undef glShaderSource
#undef glLinkProgram
#undef eglSwapBuffers
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "corpus.h"

static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLContext ectx = EGL_NO_CONTEXT;
static GLuint fbo, rb_color, rb_depth;
static const char *title_;

int corpus_backend_open(const char *title, int width, int height, int depth, int stencil)
{
    static const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
    title_ = title;
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1); /* llvmpipe, not WSL's d3d12 */
    get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!get_display)
        return 0;
    dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
        return 0;
    ectx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (ectx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ectx))
        return 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb_color);
    glBindRenderbuffer(GL_RENDERBUFFER, rb_color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb_color);
    if (depth || stencil) {
        glGenRenderbuffers(1, &rb_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, rb_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb_depth);
        if (stencil)
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb_depth);
    }
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 0;
    glViewport(0, 0, width, height); /* as a window surface's first eglMakeCurrent */
    printf("corpus: host renderer %s\n", (const char *)glGetString(GL_RENDERER));
    return 1;
}

int corpus_backend_present(void)
{
    glFinish();
    return 1;
}

void corpus_backend_hold(void)
{
}

void corpus_backend_close(void)
{
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &rb_color);
    if (rb_depth)
        glDeleteRenderbuffers(1, &rb_depth);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ectx);
    eglTerminate(dpy);
}

/* ---- shader sources ---- */

struct src
{
    GLuint shader;
    char *text;
    struct src *next;
};
static struct src *sources;

static struct src *find(GLuint shader)
{
    struct src *s;
    for (s = sources; s; s = s->next)
        if (s->shader == shader)
            return s;
    return 0;
}

/* GLES2's concatenation, as ogles2's glShaderSource does it. */
void corpus_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                           const GLint *length)
{
    struct src *s = find(shader);
    size_t n = 0, k;
    GLsizei i;
    char *t;
    glShaderSource(shader, count, string, length);
    for (i = 0; i < count; i++)
        n += length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
    if (!(t = malloc(n + 1)))
        return;
    for (i = 0, k = 0; i < count; i++) {
        size_t l = length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
        memcpy(t + k, string[i], l);
        k += l;
    }
    t[n] = 0;
    if (!s) {
        if (!(s = calloc(1, sizeof(*s)))) {
            free(t);
            return;
        }
        s->shader = shader;
        s->next = sources;
        sources = s;
    }
    free(s->text);
    s->text = t;
}

void corpus_glLinkProgram(GLuint program)
{
    const char *path = getenv("CORPUS_SHADERS");
    GLuint sh[2];
    GLsizei n = 0, i;
    const char *vs = 0, *fs = 0;
    FILE *f;
    glLinkProgram(program);
    glGetAttachedShaders(program, 2, &n, sh);
    for (i = 0; i < n; i++) {
        GLint type = 0;
        struct src *s = find(sh[i]);
        glGetShaderiv(sh[i], GL_SHADER_TYPE, &type);
        if (s && type == GL_VERTEX_SHADER)
            vs = s->text;
        if (s && type == GL_FRAGMENT_SHADER)
            fs = s->text;
    }
    if (!vs || !fs || !(f = fopen(path && *path ? path : "corpus_shaders.txt", "ab")))
        return;
    fprintf(f, "--- program %s\n--- vertex shader\n%s\n--- fragment shader\n%s\n--- end\n", title_, vs, fs);
    fclose(f);
}
