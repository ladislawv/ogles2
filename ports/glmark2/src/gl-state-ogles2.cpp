/*
 * glmark2 on AmigaOS 3 (ports/glmark2): the GL state over ogles2.library.
 * The context belongs to NativeStateAmiga (it comes with the window); the
 * GL functions are aglGetProcAddress's thunks, which call the current
 * context through the library's dispatch table.
 */
#include "gl-state-ogles2.h"
#include "gl-headers.h"
#include "gl-visual-config.h"
#include "log.h"

extern "C" {
void *aglGetProcAddress(const char *name);
void aglMakeCurrent(void *context);
void aglSwapBuffers(void);
}

static GLADapiproc
load_proc(void * /*userptr*/, const char *name)
{
    return reinterpret_cast<GLADapiproc>(aglGetProcAddress(name));
}

bool
GLStateOGLES2::init_display(void * /*native_display*/, GLVisualConfig& /*config_pref*/)
{
    return true;
}

bool
GLStateOGLES2::init_surface(void *native_window)
{
    ctx_ = native_window;
    return ctx_ != nullptr;
}

bool
GLStateOGLES2::init_gl_extensions()
{
    if (!loaded_) {
        if (!gladLoadGLES2UserPtr(load_proc, nullptr)) {
            Log::error("Loading GLESv2 entry points failed.\n");
            return false;
        }
        loaded_ = true;
    }

    GLExtensions::MapBuffer = glMapBufferOES;
    GLExtensions::UnmapBuffer = glUnmapBufferOES;

    GLExtensions::GenFramebuffers = glGenFramebuffers;
    GLExtensions::DeleteFramebuffers = glDeleteFramebuffers;
    GLExtensions::BindFramebuffer = glBindFramebuffer;
    GLExtensions::FramebufferTexture2D = glFramebufferTexture2D;
    GLExtensions::FramebufferRenderbuffer = glFramebufferRenderbuffer;
    GLExtensions::CheckFramebufferStatus = glCheckFramebufferStatus;

    GLExtensions::GenRenderbuffers = glGenRenderbuffers;
    GLExtensions::DeleteRenderbuffers = glDeleteRenderbuffers;
    GLExtensions::BindRenderbuffer = glBindRenderbuffer;
    GLExtensions::RenderbufferStorage = glRenderbufferStorage;

    GLExtensions::GenerateMipmap = glGenerateMipmap;
    return true;
}

bool
GLStateOGLES2::valid()
{
    if (!ctx_)
        return false;
    aglMakeCurrent(ctx_);
    return true;
}

bool
GLStateOGLES2::reset()
{
    return true; /* the context lives as long as the window */
}

void
GLStateOGLES2::swap()
{
    aglSwapBuffers();
}

bool
GLStateOGLES2::gotNativeConfig(intptr_t& vid, std::vector<uint64_t>& mods)
{
    vid = 0;
    mods.clear();
    return true;
}

void
GLStateOGLES2::getVisualConfig(GLVisualConfig& vc)
{
    /* What every ogles2 context has: 32-bit colour, depth 24, stencil 8. */
    vc.id = 0;
    vc.red = vc.green = vc.blue = vc.alpha = 8;
    vc.depth = 24;
    vc.stencil = 8;
    vc.buffer = 32;
    vc.samples = 0;
}
