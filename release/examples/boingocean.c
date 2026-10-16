/*
 * ogles2.library example 3: Boing Ocean. The Amiga Boing ball floating on a
 * ray-marched ocean, drawn entirely by one fragment shader.
 *
 * Shows a heavy shader on ogles2 and how to make it affordable:
 *   - everything that is the same for every pixel is worked out once a frame
 *     on the CPU and passed as uniforms: the camera, the sun, the sky light,
 *     the ball's float (the mean water height under it and the slope there)
 *     and the orientation of its checkerboard;
 *   - what is the same for many pixels is drawn once a frame into small
 *     textures (framebuffer objects) that the scene reads: the coarse water
 *     height over the sea around the ball (the ray march then takes one
 *     texture read a step instead of eight wave octaves), and a table of the
 *     sky's colour by height and angle to the sun (for every reflection);
 *   - the view ray comes from the vertex shader, interpolated across one
 *     full-screen triangle, instead of a per-pixel camera matrix;
 *   - keys 1 to 4 draw the scene at a fraction of the window's size (1/1 ..
 *     1/4; 1/1 at start) into a texture that a second, tiny shader scales up.
 * Drag with the left mouse button to turn and raise the camera; it also
 * circles the ball by itself. The window's title shows the frame rate.
 * Close the window, press Esc or Ctrl-C to quit (Esc or Ctrl-C full screen).
 *
 *   boingocean [WIDTH=w] [HEIGHT=h] [SCALE=1..4] [FRAMES=n] [FULLSCREEN]
 * FRAMES quits after n frames and prints the average frame rate.
 * FULLSCREEN opens a 32-bit screen of WIDTH x HEIGHT (640 x 480 by default)
 * that ogles2 draws straight into, with no window to blit into; input then
 * comes from the screen's backdrop window (OGLES2_CCT_PISTORM_GET_WINDOW).
 *
 * Build (see the Makefile):
 *   m68k-amigaos-gcc -O2 -fno-builtin -I../include boingocean.c -L../lib -logles2 -lm881 -o boingocean
 *
 * MIT License. The water's wave model and sky are afl_ext's "Very fast
 * procedural ocean" (https://www.shadertoy.com/view/MdXyzX, MIT License,
 * Copyright (c) 2017-2024 afl_ext); see boingocean_shaders.h.
 */
#include <stdio.h>
#include <math.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <graphics/displayinfo.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <proto/graphics.h>
#include <proto/ogles2.h>
#include "boingocean_shaders.h"

struct IntuitionBase *IntuitionBase;
struct Device *TimerBase;
struct GfxBase *GfxBase;
static struct timerequest timer_req;

#define WATER_DEPTH 1.0f
#define DRAG_MULT 0.38f
#define BALL_R 0.8f
#define CAM_DIST 4.5f
#define PI 3.14159265f

typedef struct { float x, y, z; } vec3;

static vec3 v3(float x, float y, float z) { vec3 v; v.x = x; v.y = y; v.z = z; return v; }
static vec3 add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec3 sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec3 scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static vec3 cross(vec3 a, vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static vec3 normalize(vec3 a) { return scale(a, 1.0f / (float)sqrt(dot(a, a))); }

/* The shader's wavesMarch(), on the CPU: the water height the ball floats
 * on matches the surface the GPU draws. */
static float waves(float px, float pz, float t)
{
    float phase = (float)sqrt(px * px + pz * pz) * 0.1f;
    float iter = 0.0f, freq = 1.0f, tmul = 2.0f, weight = 1.0f, sum = 0.0f, weights = 0.0f;
    int i;
    for (i = 0; i < 8; i++) { /* as the shader's wavesMarch */
        float dx = (float)sin(iter), dz = (float)cos(iter);
        float x = (dx * px + dz * pz) * freq + t * tmul + phase;
        float wave = (float)exp(sin(x) - 1.0);
        float d = -wave * (float)cos(x);
        px += dx * d * weight * DRAG_MULT;
        pz += dz * d * weight * DRAG_MULT;
        sum += wave * weight;
        weights += weight;
        weight *= 0.8f;
        freq *= 1.18f;
        tmul *= 1.07f;
        iter += 1232.399963f;
    }
    return sum / weights * WATER_DEPTH - WATER_DEPTH;
}

/* The shader's atmosphere() towards straight up: the ball's sky light. */
static vec3 sky_up(vec3 sun)
{
    float st = 1.0f / 1.1f, st2 = 1.0f / (sun.y * 11.0f + 1.0f);
    float raysundt = sun.y * sun.y;
    float sundt = (float)pow(sun.y > 0.0f ? sun.y : 0.0f, 8.0);
    float mymie = sundt * st * 0.2f;
    float k[3] = {5.5f / 22.4f, 13.0f / 22.4f, 1.0f}, c[3], o[3];
    int i;
    for (i = 0; i < 3; i++) {
        float sc = 1.0f + (((1.0f - k[i]) > 0.0f ? 1.0f - k[i] : 0.0f) - 1.0f) * st2; /* mix(1, max(0, 1-k), st2) */
        float b = k[i] * sc;
        float b2 = b - k[i] * 22.4f * 0.002f * (st - 6.0f * sun.y * sun.y);
        if (b2 < 0.0f)
            b2 = 0.0f;
        b2 *= st * (0.24f + raysundt * 0.24f);
        c[i] = sc;
        o[i] = (b2 * 1.0f + mymie * c[i]) * 0.5f; /* (1 - ray.y)^3 is 0 straight up */
    }
    return v3(o[0] * 0.6f, o[1] * 0.6f, o[2] * 0.6f);
}

struct frame_uniforms {
    vec3 sun, eye, fw, rt, up, ball, px, py, pz, ambient;
};

/* Everything the shader needs that does not change across the screen. */
static void frame_setup(struct frame_uniforms *u, float t, float yaw_drag, float my)
{
    const float s = 0.6f * BALL_R, tilt = 0.35f, turn = 0.6f;
    float h0, hx1, hx0, hz1, hz0, mean, sx, sz, yaw, pitch, ct, st, cs, ss;
    vec3 target = v3(0.0f, -0.45f * WATER_DEPTH, 0.0f), up, t1, t2;

    u->sun = normalize(v3(-0.0773502691896258f, 0.5f + (float)sin(t * 0.2f + 2.6f) * 0.45f, 0.5773502691896258f));
    u->ambient = sky_up(u->sun);

    /* The ball floats: the mean water height across its waterline, tilted
     * with the slope there, drifting a little down it. */
    h0 = waves(0.0f, 0.0f, t);
    hx1 = waves(s, 0.0f, t);
    hx0 = waves(-s, 0.0f, t);
    hz1 = waves(0.0f, s, t);
    hz0 = waves(0.0f, -s, t);
    mean = (h0 * 2.0f + hx1 + hx0 + hz1 + hz0) / 6.0f;
    sx = (hx1 - hx0) / (2.0f * s);
    sz = (hz1 - hz0) / (2.0f * s);
    up = normalize(v3(-sx * 0.7f, 1.0f, -sz * 0.7f));
    u->ball = v3(-sx * 0.08f, mean - 0.2f * BALL_R, -sz * 0.08f);

    /* Its checkerboard's axes: the ball's frame, the pattern leaning
     * (tilt) and turned so the camera sees it side on. */
    t1 = normalize(v3(up.y, -up.x, 0.0f)); /* cross(up, z) */
    t2 = cross(t1, up);
    ct = (float)cos(tilt), st = (float)sin(tilt), cs = (float)cos(turn), ss = (float)sin(turn);
    u->px = add(add(scale(t1, cs * ct), scale(up, -cs * st)), scale(t2, ss));
    u->py = add(scale(t1, st), scale(up, ct));
    u->pz = add(add(scale(t1, -ss * ct), scale(up, ss * st)), scale(t2, cs));

    /* The camera circles a fixed point (so the bobbing shows), once in 25 s. */
    yaw = yaw_drag + t * 0.25f;
    pitch = 0.12f + (0.8f - 0.12f) * my;
    u->eye = add(target, scale(v3((float)sin(yaw) * (float)cos(pitch), (float)sin(pitch),
                                  -(float)cos(yaw) * (float)cos(pitch)), CAM_DIST));
    if (u->eye.y < 0.6f)
        u->eye.y = 0.6f;
    u->fw = normalize(sub(target, u->eye));
    u->rt = normalize(v3(u->fw.z, 0.0f, -u->fw.x)); /* cross(y, fw) */
    u->up = cross(u->fw, u->rt);
}

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

static GLuint link_program(const char *vsrc, const char *fsrc)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vsrc), fs = compile(GL_FRAGMENT_SHADER, fsrc);
    GLuint p = glCreateProgram();
    GLint ok = 0;
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        printf("link: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* An RGBA8 texture to draw into (a framebuffer object), linear filtered,
 * clamped at its edges. */
static BOOL make_rt(GLuint *tex, GLuint *fbo, int w, int h)
{
    if (*fbo) {
        glDeleteFramebuffers(1, fbo);
        glDeleteTextures(1, tex);
        *fbo = *tex = 0;
    }
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("a render target (%dx%d) is not complete\n", w, h);
        return FALSE;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return TRUE;
}

/* The coarse water height and slope (the shaders' EXTENT: -30..30 in x and
 * z), drawn each frame, and the sky table, drawn every 16th frame (the sun
 * moves slowly). */
#define HEIGHT_RES 160
#define EXTENT 30.0f
#define SKY_RES 64
static GLuint height_tex, height_fbo, sky_tex, sky_fbo;

/* The texture the scene is drawn into at the window's size / div; none at
 * 1/1, where it goes to the window directly. */
static GLuint target_tex, target_fbo;
static int target_w, target_h;

static BOOL make_target(int w, int h, int div)
{
    target_w = (w + div - 1) / div;
    target_h = (h + div - 1) / div;
    if (target_w < 1)
        target_w = 1;
    if (target_h < 1)
        target_h = 1;
    if (div == 1) {
        if (target_fbo) {
            glDeleteFramebuffers(1, &target_fbo);
            glDeleteTextures(1, &target_tex);
            target_fbo = target_tex = 0;
        }
        return TRUE;
    }
    return make_rt(&target_tex, &target_fbo, target_w, target_h);
}

static double now_s(void)
{
    /* struct TimeVal: the same fields with every compiler's C library
     * (a POSIX one may define its own struct timeval). */
    struct TimeVal tv;
    GetSysTime((TimeVal_Type *)&tv);
    return tv.tv_secs + tv.tv_micro / 1e6;
}

struct input {
    BOOL quit, resized, dragging;
    int div;           /* render scale: the window's size / div */
    WORD last_x, last_y;
    float yaw, my;     /* the camera, from dragging */
};

static void events(struct Window *w, struct input *in)
{
    struct IntuiMessage *m;
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        in->quit = TRUE;
    while ((m = (struct IntuiMessage *)GetMsg(w->UserPort))) {
        switch (m->Class) {
        case IDCMP_CLOSEWINDOW:
            in->quit = TRUE;
            break;
        case IDCMP_VANILLAKEY:
            if (m->Code == 27)
                in->quit = TRUE;
            else if (m->Code >= '1' && m->Code <= '4') {
                in->div = m->Code - '0';
                in->resized = TRUE;
            }
            break;
        case IDCMP_NEWSIZE:
            in->resized = TRUE;
            break;
        case IDCMP_MOUSEBUTTONS:
            in->dragging = m->Code == SELECTDOWN;
            in->last_x = m->MouseX;
            in->last_y = m->MouseY;
            break;
        case IDCMP_MOUSEMOVE:
            if (in->dragging) {
                in->yaw += (m->MouseX - in->last_x) * 0.01f;
                in->my -= (m->MouseY - in->last_y) * 0.004f;
                if (in->my < 0.0f)
                    in->my = 0.0f;
                if (in->my > 1.0f)
                    in->my = 1.0f;
                in->last_x = m->MouseX;
                in->last_y = m->MouseY;
            }
            break;
        }
        ReplyMsg((struct Message *)m);
    }
}

int main(void)
{
    static const GLfloat tri[] = {-1, -1, 3, -1, -1, 3}; /* covers the screen */
    static char title[80];
    LONG args[5] = {0, 0, 0, 0, 0};
    struct RDArgs *rda;
    struct Window *win = NULL, *iw = NULL; /* our window; the input window */
    void *ctx = NULL;
    BOOL full;
    ULONG err = 0, width = 0, height = 0;
    GLuint sea = 0, blit = 0, hprog = 0, sprog = 0, buf = 0;
    GLint u_time, u_sun, u_eye, u_ball, u_px, u_py, u_pz, u_ambient, u_fw, u_rt, u_up, u_aspect;
    GLint uh_time, us_sun;
    struct input in = {FALSE, FALSE, FALSE, 1, 0, 0, 0.0f, 0.3f};
    struct frame_uniforms fu;
    double start, last_title;
    unsigned long frames = 0, title_frames = 0, max_frames = 0;
    int w0 = 640, h0 = 480, rc = 20;
    BOOL timer_open = FALSE;

    rda = ReadArgs((STRPTR) "WIDTH/N,HEIGHT/N,SCALE/N,FRAMES/N,FULLSCREEN/S", args, NULL);
    if (!rda) {
        printf("usage: boingocean [WIDTH=w] [HEIGHT=h] [SCALE=1..4] [FRAMES=n] [FULLSCREEN]\n");
        return 20;
    }
    if (args[0])
        w0 = *(LONG *)args[0];
    if (args[1])
        h0 = *(LONG *)args[1];
    if (args[2] && *(LONG *)args[2] >= 1 && *(LONG *)args[2] <= 4)
        in.div = *(LONG *)args[2];
    if (args[3])
        max_frames = *(LONG *)args[3];
    full = args[4] != 0;
    FreeArgs(rda);

    if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39))) {
        printf("needs intuition.library 39\n");
        return 20;
    }
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&timer_req, 0)) {
        printf("needs timer.device\n");
        goto out;
    }
    timer_open = TRUE;
    TimerBase = timer_req.tr_node.io_Device;
    if (!OGLES2Open()) {
        printf("needs %s 1.3 or newer: in LIBS:, or in this program's drawer or its libs/\n"
               "(an older one that another program is using cannot be replaced)\n", OGLES2NAME);
        goto out;
    }
    if (full) {
        /* A true-colour mode of the size asked for, which ogles2 opens as a
         * screen. graphics.library cannot tell a 24-bit mode (3 bytes a
         * pixel) from a 32-bit one, and ogles2 takes only 32-bit ones: try
         * each mode of that size with 24 or more bits until ogles2 accepts
         * one. (An ASL screen-mode requester is the friendlier way.) */
        ULONG id = INVALID_ID;
        if (!(GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39))) {
            printf("FULLSCREEN needs graphics.library 39\n");
            goto out;
        }
        while (!ctx && (id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
            struct DimensionInfo dims;
            if (!GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof(dims), DTAG_DIMS, id) || dims.MaxDepth < 24 ||
                dims.Nominal.MaxX - dims.Nominal.MinX + 1 != w0 || dims.Nominal.MaxY - dims.Nominal.MinY + 1 != h0)
                continue;
            ctx = aglCreateContextTags2(&err, OGLES2_CCT_MODEID, id, OGLES2_CCT_PISTORM_GET_WINDOW, (ULONG)&iw,
                                        OGLES2_CCT_DEPTH, 0, OGLES2_CCT_STENCIL, 0, OGLES2_CCT_GET_WIDTH,
                                        (ULONG)&width, OGLES2_CCT_GET_HEIGHT, (ULONG)&height, TAG_DONE);
        }
        if (!ctx || !iw) {
            printf("no %dx%d screen mode that ogles2 can draw into (it needs a 32-bit one)\n", w0, h0);
            goto out;
        }
        /* The backdrop window has no messages until asked for. */
        ModifyIDCMP(iw, IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE);
        iw->Flags |= WFLG_REPORTMOUSE | WFLG_RMBTRAP;
    } else {
    win = OpenWindowTags(NULL, WA_Title, (ULONG) "ogles2: Boing Ocean", WA_InnerWidth, w0, WA_InnerHeight, h0,
                         WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_SizeGadget, TRUE,
                         WA_SizeBBottom, TRUE, WA_MinWidth, 100, WA_MinHeight, 80, WA_MaxWidth, -1,
                         WA_MaxHeight, -1, WA_Activate, TRUE, WA_ReportMouse, TRUE, WA_RMBTrap, TRUE,
                         WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_NEWSIZE | IDCMP_MOUSEBUTTONS |
                             IDCMP_MOUSEMOVE,
                         TAG_DONE);
    if (!win) {
        printf("cannot open a window\n");
        goto out;
    }
    /* No depth or stencil buffer: the shader does all the depth there is. */
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_WINDOW, (ULONG)win, OGLES2_CCT_DEPTH, 0, OGLES2_CCT_STENCIL, 0,
                                OGLES2_CCT_GET_WIDTH, (ULONG)&width, OGLES2_CCT_GET_HEIGHT, (ULONG)&height,
                                TAG_DONE);
    if (!ctx) {
        printf("aglCreateContextTags2 failed (error %lu)\n", (unsigned long)err);
        goto out;
    }
    iw = win;
    }
    aglMakeCurrent(ctx);
    if (!(sea = link_program(boingocean_vs, boingocean_fs)) || !(blit = link_program(blit_vs, blit_fs)) ||
        !(hprog = link_program(blit_vs, height_fs)) || !(sprog = link_program(blit_vs, sky_fs)))
        goto out;
    glUseProgram(hprog);
    uh_time = glGetUniformLocation(hprog, "u_time");
    glUniform1f(glGetUniformLocation(hprog, "u_texel"), 2.0f * EXTENT / HEIGHT_RES);
    glUniform1f(glGetUniformLocation(hprog, "u_extent"), EXTENT);
    glUseProgram(sprog);
    us_sun = glGetUniformLocation(sprog, "u_sun");
    glUseProgram(sea);
    glUniform1i(glGetUniformLocation(sea, "u_height"), 0);
    glUniform1i(glGetUniformLocation(sea, "u_sky"), 1);
    u_time = glGetUniformLocation(sea, "u_time");
    u_sun = glGetUniformLocation(sea, "u_sun");
    u_eye = glGetUniformLocation(sea, "u_eye");
    u_ball = glGetUniformLocation(sea, "u_ball");
    u_px = glGetUniformLocation(sea, "u_px");
    u_py = glGetUniformLocation(sea, "u_py");
    u_pz = glGetUniformLocation(sea, "u_pz");
    u_ambient = glGetUniformLocation(sea, "u_ambient");
    u_fw = glGetUniformLocation(sea, "u_fw");
    u_rt = glGetUniformLocation(sea, "u_rt");
    u_up = glGetUniformLocation(sea, "u_up");
    u_aspect = glGetUniformLocation(sea, "u_aspect");
    glUseProgram(blit);
    glUniform1i(glGetUniformLocation(blit, "u_tex"), 0);

    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
    glEnableVertexAttribArray(0);
    if (!make_rt(&height_tex, &height_fbo, HEIGHT_RES, HEIGHT_RES) || !make_rt(&sky_tex, &sky_fbo, SKY_RES, SKY_RES) ||
        !make_target(width, height, in.div))
        goto out;

    start = last_title = now_s();
    while (!in.quit) {
        double t;
        events(iw, &in);
        if (in.resized && full) {
            /* A screen does not change size: only the render scale. */
            if (!make_target(width, height, in.div))
                break;
            in.resized = FALSE;
        } else if (in.resized) {
            /* The surface takes the window's new inner size, the render
             * target the new fraction of it. */
            aglSetParamsTags2(OGLES2_CCT_WINDOW, (ULONG)win, OGLES2_CCT_GET_WIDTH, (ULONG)&width,
                              OGLES2_CCT_GET_HEIGHT, (ULONG)&height, TAG_DONE);
            if (!make_target(width, height, in.div))
                break;
            in.resized = FALSE;
        }
        t = now_s() - start;
        frame_setup(&fu, (float)t, in.yaw, in.my);

        /* The coarse water height and slope, then (now and then) the sky
         * table. */
        glBindFramebuffer(GL_FRAMEBUFFER, height_fbo);
        glViewport(0, 0, HEIGHT_RES, HEIGHT_RES);
        glUseProgram(hprog);
        glUniform1f(uh_time, (float)t);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if ((frames & 15) == 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, sky_fbo);
            glViewport(0, 0, SKY_RES, SKY_RES);
            glUseProgram(sprog);
            glUniform3f(us_sun, fu.sun.x, fu.sun.y, fu.sun.z);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        /* The scene: into the window, or into the small target. */
        glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
        glViewport(0, 0, target_w, target_h);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sky_tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, height_tex);
        glUseProgram(sea);
        glUniform1f(u_time, (float)t);
        glUniform3f(u_sun, fu.sun.x, fu.sun.y, fu.sun.z);
        glUniform3f(u_eye, fu.eye.x, fu.eye.y, fu.eye.z);
        glUniform3f(u_ball, fu.ball.x, fu.ball.y, fu.ball.z);
        glUniform3f(u_px, fu.px.x, fu.px.y, fu.px.z);
        glUniform3f(u_py, fu.py.x, fu.py.y, fu.py.z);
        glUniform3f(u_pz, fu.pz.x, fu.pz.y, fu.pz.z);
        glUniform3f(u_ambient, fu.ambient.x, fu.ambient.y, fu.ambient.z);
        glUniform3f(u_fw, fu.fw.x, fu.fw.y, fu.fw.z);
        glUniform3f(u_rt, fu.rt.x, fu.rt.y, fu.rt.z);
        glUniform3f(u_up, fu.up.x, fu.up.y, fu.up.z);
        glUniform1f(u_aspect, (float)target_w / (float)target_h);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        /* Scaled up to the window. */
        if (target_fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, width, height);
            glUseProgram(blit);
            glBindTexture(GL_TEXTURE_2D, target_tex);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        aglSwapBuffers();

        frames++;
        title_frames++;
        if (!full && now_s() - last_title >= 1.0) {
            double n = now_s();
            sprintf(title, "ogles2: Boing Ocean  %.1f fps  %dx%d", title_frames / (n - last_title), target_w,
                    target_h);
            SetWindowTitles(win, (UBYTE *)title, (UBYTE *)-1);
            last_title = n;
            title_frames = 0;
        }
        if (max_frames && frames >= max_frames)
            break;
    }
    if (frames) {
        double s = now_s() - start;
        printf("%lu frames in %.1f s: %.2f fps at %dx%d (%s %lux%lu)\n", frames, s, frames / s, target_w, target_h,
               full ? "screen" : "window", (unsigned long)width, (unsigned long)height);
    }
    rc = 0;
out:
    if (ctx) {
        if (target_fbo)
            glDeleteFramebuffers(1, &target_fbo);
        if (target_tex)
            glDeleteTextures(1, &target_tex);
        if (height_fbo)
            glDeleteFramebuffers(1, &height_fbo);
        if (height_tex)
            glDeleteTextures(1, &height_tex);
        if (sky_fbo)
            glDeleteFramebuffers(1, &sky_fbo);
        if (sky_tex)
            glDeleteTextures(1, &sky_tex);
        if (hprog)
            glDeleteProgram(hprog);
        if (sprog)
            glDeleteProgram(sprog);
        if (buf)
            glDeleteBuffers(1, &buf);
        if (sea)
            glDeleteProgram(sea);
        if (blit)
            glDeleteProgram(blit);
        aglDestroyContext(ctx);
    }
    if (win)
        CloseWindow(win);
    OGLES2Close();
    if (timer_open)
        CloseDevice((struct IORequest *)&timer_req);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
