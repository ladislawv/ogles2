/*
 * sdl3_gles2: SDL3 for the window and input, your own OpenGL ES 2.0 for the
 * drawing: an ES 2.0 context from SDL (which selects ogles2.library), a
 * shader program, a vertex buffer, a spinning triangle, vsync.
 *   usage: sdl3_gles2 [seconds]     (Esc or the close gadget quits)
 * Prints the frame rate at the end. The shaders are compiled at run time:
 * the first run needs the shader compiler (LIBS:v3dglslc) and caches them in
 * OGLES2Cache: (or T:). The GL functions come from <proto/ogles2.h> and
 * libogles2.a; portable code may look them up with SDL_GL_GetProcAddress.
 *
 * MIT licence (the ogles2 project's LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include <proto/ogles2.h>

long __stack = 65536; /* clib2: the program's stack */
static const char version[] __attribute__((used)) = "$VER: sdl3_gles2 1.0 (16.10.2026)";

static const char vs_src[] =
    "attribute vec2 a_pos;\n"
    "attribute vec3 a_col;\n"
    "uniform float u_angle;\n"
    "uniform float u_aspect;\n"
    "varying vec3 v_col;\n"
    "void main() {\n"
    "    float c = cos(u_angle), s = sin(u_angle);\n"
    "    vec2 p = vec2(c * a_pos.x - s * a_pos.y, s * a_pos.x + c * a_pos.y);\n"
    "    gl_Position = vec4(p.x / u_aspect, p.y, 0.0, 1.0);\n"
    "    v_col = a_col;\n"
    "}\n";

static const char fs_src[] =
    "precision mediump float;\n"
    "varying vec3 v_col;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(v_col, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("shader: %s\n", log);
    }
    return s;
}

int main(int argc, char **argv)
{
    static const GLfloat verts[] = {
        /* x, y, r, g, b */
        0.0f, 0.8f, 1.0f, 0.2f, 0.2f,
        -0.7f, -0.5f, 0.2f, 1.0f, 0.2f,
        0.7f, -0.5f, 0.2f, 0.4f, 1.0f,
    };
    SDL_Window *win;
    SDL_GLContext ctx;
    GLuint prog, vbo;
    GLint ok = 0, u_angle, u_aspect;
    Uint64 start, now, limit = argc > 1 ? (Uint64)atoi(argv[1]) * 1000 : 0;
    unsigned frames = 0;
    bool running = true;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("SDL_Init: %s\n", SDL_GetError());
        return 20;
    }
    /* OpenGL ES 2.0: the request that selects ogles2.library (anything else
     * goes to minigl.library). No depth or stencil buffer: none needed. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

    win = SDL_CreateWindow("sdl3_gles2", 640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    ctx = win ? SDL_GL_CreateContext(win) : NULL;
    if (!ctx) {
        printf("sdl3_gles2: %s\n", SDL_GetError());
        SDL_Quit();
        return 20;
    }
    SDL_GL_SetSwapInterval(1); /* vsync */
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));

    prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_col");
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        printf("sdl3_gles2: the program did not link\n");
        SDL_GL_DestroyContext(ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 20;
    }
    glUseProgram(prog);
    u_angle = glGetUniformLocation(prog, "u_angle");
    u_aspect = glGetUniformLocation(prog, "u_aspect");

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (const void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));
    glClearColor(0.08f, 0.08f, 0.15f, 1.0f);

    start = SDL_GetTicks();
    while (running) {
        SDL_Event e;
        int w, h;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE))
                running = false;
        }
        now = SDL_GetTicks();
        if (limit && now - start >= limit)
            running = false;

        /* The window may have been resized: SDL and ogles2 keep the context. */
        SDL_GetWindowSizeInPixels(win, &w, &h);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1f(u_angle, (now - start) / 1000.0f);
        glUniform1f(u_aspect, h > 0 ? (float)w / (float)h : 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        SDL_GL_SwapWindow(win);
        frames++;
    }
    now = SDL_GetTicks();
    if (now > start)
        printf("%u frames in %.1f s: %.1f fps\n", frames, (now - start) / 1000.0,
               frames * 1000.0 / (double)(now - start));

    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
