/* Smoke test for stream B: GLES2 on Mesa's v3d driver through the drm-shim. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>

static const char *vs_src =
    "attribute vec4 pos;\n"
    "uniform mat4 mvp;\n"
    "void main() { gl_Position = mvp * pos; }\n";
static const char *fs_src =
    "precision mediump float;\n"
    "uniform vec4 color;\n"
    "void main() { gl_FragColor = color; }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    char log[1024] = "";
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    printf("compile %s: %s %s\n", type == GL_VERTEX_SHADER ? "VS" : "FS", ok ? "ok" : "FAILED", log);
    return s;
}

int main(void)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC getdpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getdpy(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint major, minor, n;
    if (!eglInitialize(dpy, &major, &minor)) { printf("eglInitialize failed 0x%x\n", eglGetError()); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig cfg;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) { printf("no config\n"); return 1; }
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    EGLint pb_attr[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) { printf("makecurrent failed 0x%x\n", eglGetError()); return 1; }

    printf("GL_RENDERER: %s\nGL_VERSION: %s\nGLSL: %s\n", glGetString(GL_RENDERER),
           glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

    GLuint p = glCreateProgram();
    GLint ok = 0;
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs_src));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    printf("link: %s\n", ok ? "ok" : "FAILED");
    glUseProgram(p);

    static const GLfloat tri[] = { -1, -1, 0, 1,  1, -1, 0, 1,  0, 1, 0, 1 };
    static const GLfloat ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    glUniformMatrix4fv(glGetUniformLocation(p, "mvp"), 1, GL_FALSE, ident);
    glUniform4f(glGetUniformLocation(p, "color"), 1, 0.5f, 0, 1);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    printf("draw done, glGetError=0x%x\n", glGetError());
    return 0;
}
