/*
 * glmark2 on AmigaOS 3 (ports/glmark2): the GL state over ogles2.library.
 * NativeStateAmiga makes the context (it comes with the window); this makes
 * it current, loads the GL functions through aglGetProcAddress (context-free
 * thunks into the library's dispatch table) and swaps.
 */
#ifndef GLMARK2_GL_STATE_OGLES2_H_
#define GLMARK2_GL_STATE_OGLES2_H_

#include "gl-state.h"

class GLStateOGLES2 : public GLState
{
public:
    GLStateOGLES2() : ctx_(nullptr), loaded_(false) {}

    bool init_display(void *native_display, GLVisualConfig& config_pref);
    bool init_surface(void *native_window);
    bool init_gl_extensions();
    bool valid();
    bool reset();
    void swap();
    bool gotNativeConfig(intptr_t& vid, std::vector<uint64_t>& mods);
    void getVisualConfig(GLVisualConfig& vc);
    bool supports_sync() { return false; }
    std::unique_ptr<GLStateSync> sync() { return nullptr; }

private:
    void *ctx_;
    bool loaded_;
};

#endif /* GLMARK2_GL_STATE_OGLES2_H_ */
