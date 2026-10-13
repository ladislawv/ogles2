/*
 * glmark2 on AmigaOS 3 (ports/glmark2): the native state. The window is the
 * one ogles2.library opens with the context (on the Workbench screen), so
 * the context is made here and handed to GLStateOGLES2 as the "window".
 */
#ifndef GLMARK2_NATIVE_STATE_AMIGA_H_
#define GLMARK2_NATIVE_STATE_AMIGA_H_

#include "native-state.h"

struct Window;

class NativeStateAmiga : public NativeState
{
public:
    NativeStateAmiga();
    ~NativeStateAmiga();

    bool init_display();
    void* display();
    bool create_window(WindowProperties const& properties);
    void* window(WindowProperties& properties);
    void visible(bool v);
    bool should_quit();
    void flip();

private:
    void destroy_window();

    void *ctx_;
    struct Window *win_;
    bool lib_open_;
    bool quit_;
    WindowProperties properties_;
};

#endif /* GLMARK2_NATIVE_STATE_AMIGA_H_ */
