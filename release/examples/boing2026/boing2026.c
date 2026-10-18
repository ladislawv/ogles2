/*
 * Boing 2026: the Amiga's 1984 Boing Ball, remade for ogles2.library.
 *
 * A glossy, per-pixel lit checkered ball bounces in a neon-gridded room:
 *   - a real-time soft shadow: a shadow map in a depth texture
 *     (OES_depth_texture), 12 rotated PCF taps, wider as the ball rises;
 *   - a polished floor that reflects the ball (render to texture, the ball
 *     drawn mirrored) and the back wall's grid (traced in the shader);
 *   - bloom on the highlights and the neon (a bright pass at 1/4 size,
 *     two separable Gaussian blurs, added on top);
 *   - 4x MSAA on the window (OGLES2_CCT_PISTORM_MSAA);
 *   - the checkerboard computed per pixel and antialiased with fwidth
 *     (OES_standard_derivatives); squash and stretch; a synthesised "boing"
 *     on Paula.
 * The room is a box seen from inside and the ball is convex, so no pass needs
 * a depth buffer except the shadow map.
 *
 * Keys: 1 shadow  2 reflection  3 bloom  4 per-pixel/Gouraud lighting
 *       5 checker antialiasing  6 classic 1984 look  M the shadow map and
 *       the shadow term alone  L stop/start the light  Space pause  T time each pass (printed to the
 *       Shell)  Esc quit
 * Drag with the left mouse button to turn and raise the camera.
 *
 *   boing2026 [WIDTH=w] [HEIGHT=h] [FRAMES=n] [NOMSAA] [NOSOUND] [FULLSCREEN] [TIMING]
 *             [SHOWMAP] [VSYNC]
 * TIMING times each pass every 120 frames, as T does; SHOWMAP starts as M;
 * Frames are not limited: VSYNC waits for the display (no tearing). TOUR switches the
 * effects off and on one by one, saying in the title what each proves.
 *
 * Built on the ogles2 SDK's Boing Ocean example. MIT License.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <devices/audio.h>
#include <graphics/displayinfo.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <proto/graphics.h>
#include <proto/ogles2.h>
#include "boing2026_shaders.h"

struct IntuitionBase *IntuitionBase;
struct Device *TimerBase;
struct GfxBase *GfxBase;
static struct timerequest timer_req;

#define PI 3.14159265f
#define BALL_R 1.0f
#define ROOM_X 5.5f     /* side walls at +-ROOM_X */
#define ROOM_H 7.0f
#define ROOM_BACK -5.0f
#define ROOM_FRONT 13.0f
#define PEAK 2.9f       /* bounce height above resting */
#define BOUNCE_T 1.35f  /* seconds a bounce */
#define SPEED_X 2.1f
#define SHADOW_RES 512
#define SHADOW_HALF 8.0f /* the shadow map covers 16 x 16 units */
#define BLOOM 1.5f       /* the bloom's strength */

/* ---- vectors and column-major 4x4 matrices ---- */
typedef struct { float x, y, z; } vec3;
typedef struct { float m[16]; } mat4;

static vec3 v3(float x, float y, float z) { vec3 v; v.x = x; v.y = y; v.z = z; return v; }
static vec3 sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static vec3 cross(vec3 a, vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static vec3 normalize(vec3 a)
{
    float s = 1.0f / (float)sqrt(dot(a, a));
    return v3(a.x * s, a.y * s, a.z * s);
}

static mat4 identity(void)
{
    mat4 r;
    memset(&r, 0, sizeof(r));
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

static mat4 mul(mat4 a, mat4 b)
{
    mat4 r;
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++)
                s += a.m[k * 4 + j] * b.m[i * 4 + k];
            r.m[i * 4 + j] = s;
        }
    return r;
}

static mat4 translate(vec3 t)
{
    mat4 r = identity();
    r.m[12] = t.x, r.m[13] = t.y, r.m[14] = t.z;
    return r;
}

static mat4 scale3(float x, float y, float z)
{
    mat4 r = identity();
    r.m[0] = x, r.m[5] = y, r.m[10] = z;
    return r;
}

static mat4 rot_y(float a)
{
    mat4 r = identity();
    float c = (float)cos(a), s = (float)sin(a);
    r.m[0] = c, r.m[2] = -s, r.m[8] = s, r.m[10] = c;
    return r;
}

static mat4 rot_z(float a)
{
    mat4 r = identity();
    float c = (float)cos(a), s = (float)sin(a);
    r.m[0] = c, r.m[1] = s, r.m[4] = -s, r.m[5] = c;
    return r;
}

static mat4 perspective(float fovy, float aspect, float n, float f)
{
    mat4 r;
    float t = 1.0f / (float)tan(fovy * 0.5f);
    memset(&r, 0, sizeof(r));
    r.m[0] = t / aspect;
    r.m[5] = t;
    r.m[10] = (f + n) / (n - f);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * f * n / (n - f);
    return r;
}

static mat4 ortho(float half, float n, float f)
{
    mat4 r = identity();
    r.m[0] = r.m[5] = 1.0f / half;
    r.m[10] = -2.0f / (f - n);
    r.m[14] = -(f + n) / (f - n);
    return r;
}

static mat4 look_at(vec3 eye, vec3 at, vec3 up)
{
    vec3 f = normalize(sub(at, eye)), s = normalize(cross(f, up)), u = cross(s, f);
    mat4 r = identity();
    r.m[0] = s.x, r.m[4] = s.y, r.m[8] = s.z;
    r.m[1] = u.x, r.m[5] = u.y, r.m[9] = u.z;
    r.m[2] = -f.x, r.m[6] = -f.y, r.m[10] = -f.z;
    r.m[12] = -dot(s, eye), r.m[13] = -dot(u, eye), r.m[14] = dot(f, eye);
    return r;
}

/* The upper 3x3, for normals of a matrix whose scaling was undone. */
static void upper3(const mat4 *a, float *o)
{
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            o[i * 3 + j] = a->m[i * 4 + j];
}

/* ---- the scene's state ---- */
struct scene {
    float x, vx;         /* unsquashed centre x and its speed */
    float phase;         /* 0..1 through a bounce */
    float spin;          /* the checkerboard's turn */
    vec3 centre;         /* the ball's centre after squashing */
    float sx, sy;        /* squash: x across the walls, y down to the floor */
    mat4 model, nrm;     /* ball: world from object; normals */
    BOOL hit_floor, hit_wall;
};

static void scene_step(struct scene *s, float dt)
{
    const float lim = ROOM_X - BALL_R, over = 0.22f;
    float y, h, ax, ay, az;
    BOOL was_down = s->sy < 1.0f, was_wall = s->sx < 1.0f;
    mat4 r;

    s->x += s->vx * dt;
    if (s->x > lim + over)
        s->x = 2.0f * (lim + over) - s->x, s->vx = -s->vx;
    if (s->x < -lim - over)
        s->x = -2.0f * (lim + over) - s->x, s->vx = -s->vx;
    s->phase += dt / BOUNCE_T;
    s->phase -= (float)floor(s->phase);
    s->spin += (s->vx > 0.0f ? -1.0f : 1.0f) * dt * 2.4f;

    /* The ball's path runs a little into the floor and walls; there it
     * squashes instead, keeping its volume. */
    y = BALL_R - over + (PEAK + over) * 4.0f * s->phase * (1.0f - s->phase);
    s->sy = y < BALL_R ? y / BALL_R : 1.0f;
    h = ROOM_X - (float)fabs(s->x);
    s->sx = h < BALL_R ? h / BALL_R : 1.0f;
    s->hit_floor = !was_down && s->sy < 1.0f;
    s->hit_wall = !was_wall && s->sx < 1.0f;

    /* Squashed along the world's axes (the other two bulge, the volume
     * stays), after the classic tilt and spin; the centre keeps the
     * squashed side against the floor or wall. */
    ax = s->sx / (float)sqrt(s->sy);
    ay = s->sy / (float)sqrt(s->sx);
    az = 1.0f / (float)sqrt(s->sx * s->sy);
    s->centre = v3(s->x, y, 0.0f);
    if (s->sy < 1.0f)
        s->centre.y = ay * BALL_R;
    if (s->sx < 1.0f)
        s->centre.x = (s->x > 0.0f ? 1.0f : -1.0f) * (ROOM_X - ax * BALL_R);
    r = mul(rot_z(0.30f), rot_y(s->spin));
    s->model = mul(translate(s->centre), mul(scale3(ax, ay, az), r));
    s->nrm = mul(scale3(1.0f / ax, 1.0f / ay, 1.0f / az), r);
}

/* ---- GL helpers ---- */
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
    glBindAttribLocation(p, 1, "a_nrm");
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

static void tex_params(GLenum filter)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

struct rt {
    GLuint tex, fbo;
    int w, h;
};

static BOOL make_rt(struct rt *r, int w, int h)
{
    r->w = w, r->h = h;
    glGenTextures(1, &r->tex);
    glBindTexture(GL_TEXTURE_2D, r->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    tex_params(GL_LINEAR);
    glGenFramebuffers(1, &r->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("a %dx%d render target is not complete\n", w, h);
        return FALSE;
    }
    return TRUE;
}

static void free_rt(struct rt *r)
{
    if (r->fbo)
        glDeleteFramebuffers(1, &r->fbo);
    if (r->tex)
        glDeleteTextures(1, &r->tex);
    r->fbo = r->tex = 0;
}

/* The shadow maps, one a light: a depth texture; a colour renderbuffer only
 * if the framebuffer is not complete without one. */
static GLuint shadow_tex[2], shadow_fbo[2], shadow_rb[2];

static BOOL make_shadow(int k)
{
    glGenTextures(1, &shadow_tex[k]);
    glBindTexture(GL_TEXTURE_2D, shadow_tex[k]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, SHADOW_RES, SHADOW_RES, 0, GL_DEPTH_COMPONENT,
                 GL_UNSIGNED_SHORT, NULL);
    tex_params(GL_NEAREST);
    glGenFramebuffers(1, &shadow_fbo[k]);
    glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo[k]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_tex[k], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glGenRenderbuffers(1, &shadow_rb[k]);
        glBindRenderbuffer(GL_RENDERBUFFER, shadow_rb[k]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, SHADOW_RES, SHADOW_RES);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, shadow_rb[k]);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            printf("the shadow map's framebuffer is not complete\n");
            return FALSE;
        }
    }
    return TRUE;
}

/* After a pass: its depth (and for the shadow map its colour) need not be
 * stored from the GPU's tile memory. */
static void discard(GLsizei n, GLenum a, GLenum b)
{
    GLenum att[2];
    att[0] = a, att[1] = b;
    glDiscardFramebufferEXT(GL_FRAMEBUFFER, n, att);
}

/* ---- geometry ---- */
#define SEG_U 48
#define SEG_V 32
static GLuint sphere_vbo, sphere_ibo, room_vbo, tri_vbo, quad_vbo;
static int sphere_count;

static void make_sphere(void)
{
    static GLfloat v[(SEG_U + 1) * (SEG_V + 1) * 3];
    static GLushort idx[SEG_U * SEG_V * 6];
    int i, j, n = 0, k = 0;
    for (j = 0; j <= SEG_V; j++) {
        float lat = PI * ((float)j / SEG_V - 0.5f);
        for (i = 0; i <= SEG_U; i++) {
            float lon = 2.0f * PI * (float)i / SEG_U;
            v[n++] = (float)(cos(lat) * cos(lon));
            v[n++] = (float)sin(lat);
            v[n++] = (float)(cos(lat) * sin(lon));
        }
    }
    /* Counter-clockwise seen from outside. */
    for (j = 0; j < SEG_V; j++)
        for (i = 0; i < SEG_U; i++) {
            GLushort a = j * (SEG_U + 1) + i, b = a + 1, c = a + SEG_U + 1, d = c + 1;
            idx[k++] = a, idx[k++] = c, idx[k++] = b;
            idx[k++] = b, idx[k++] = c, idx[k++] = d;
        }
    sphere_count = k;
    glGenBuffers(1, &sphere_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glGenBuffers(1, &sphere_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphere_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
}

/* The room: five inward-facing walls, two triangles each, position and
 * normal per vertex. */
static void make_room(void)
{
#define X ROOM_X
#define H ROOM_H
#define B ROOM_BACK
#define F ROOM_FRONT
    static const float q[5][4][3] = {
        {{-X, 0, F}, {X, 0, F}, {X, 0, B}, {-X, 0, B}},     /* floor */
        {{-X, H, B}, {X, H, B}, {X, H, F}, {-X, H, F}},     /* ceiling */
        {{-X, 0, B}, {X, 0, B}, {X, H, B}, {-X, H, B}},     /* back */
        {{-X, 0, F}, {-X, 0, B}, {-X, H, B}, {-X, H, F}},   /* left */
        {{X, 0, B}, {X, 0, F}, {X, H, F}, {X, H, B}},       /* right */
    };
    static const float nrm[5][3] = {{0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {1, 0, 0}, {-1, 0, 0}};
    static const int order[6] = {0, 1, 2, 0, 2, 3};
    static GLfloat v[5 * 6 * 6];
    int w, i, n = 0;
    for (w = 0; w < 5; w++)
        for (i = 0; i < 6; i++) {
            v[n++] = q[w][order[i]][0], v[n++] = q[w][order[i]][1], v[n++] = q[w][order[i]][2];
            v[n++] = nrm[w][0], v[n++] = nrm[w][1], v[n++] = nrm[w][2];
        }
    glGenBuffers(1, &room_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, room_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
#undef X
#undef H
#undef B
#undef F
}

/* ---- the boing: synthesised once, played on a left and a right channel ---- */
#define SND_RATE 16000
#define SND_LEN 7200
static BYTE *snd;
static struct MsgPort *snd_port;
static struct IOAudio *snd_alloc, snd_io[2];
static BOOL snd_busy[2];

static void synth_boing(BYTE *s)
{
    double ph = 0.0, seed = 1.0;
    int i;
    for (i = 0; i < SND_LEN; i++) {
        double t = (double)i / SND_RATE;
        double f = (88.0 + 150.0 * exp(-t * 12.0)) * (1.0 + 0.035 * sin(2.0 * PI * 7.0 * t));
        double env = (1.0 - exp(-t * 600.0)) * exp(-t * 6.5);
        double v, noise;
        seed = fmod(seed * 16807.0, 2147483647.0);
        noise = seed / 2147483647.0 * 2.0 - 1.0;
        ph += 2.0 * PI * f / SND_RATE;
        v = env * (0.62 * sin(ph) + 0.25 * sin(2.0 * ph + 0.5) + 0.10 * sin(3.01 * ph)) + noise * exp(-t * 90.0) * 0.4;
        v = tanh(v * 1.6) * 127.0;
        s[i] = (BYTE)(v > 127.0 ? 127 : v < -128.0 ? -128 : (int)v);
    }
}

static void begin_io(struct IORequest *io)
{
    register struct IORequest *a1 __asm("a1") = io;
    register struct Device *a6 __asm("a6") = io->io_Device;
    /* BeginIO, the device's vector -30: a1 the request, a6 the device;
     * d0/d1/a0/a1 are the callee's to change. */
    __asm volatile("jsr -30(a6)" : "+r"(a1) : "r"(a6) : "d0", "d1", "a0", "cc", "memory");
}

static BOOL sound_open(void)
{
    static UBYTE chans[] = {3, 5, 10, 12}; /* a left and a right channel */
    ULONG units;
    if (!(snd = AllocMem(SND_LEN, MEMF_CHIP)))
        return FALSE;
    synth_boing(snd);
    if (!(snd_port = CreateMsgPort()))
        return FALSE;
    if (!(snd_alloc = (struct IOAudio *)CreateIORequest(snd_port, sizeof(struct IOAudio))))
        return FALSE;
    snd_alloc->ioa_Request.io_Message.mn_Node.ln_Pri = 0;
    snd_alloc->ioa_Request.io_Command = ADCMD_ALLOCATE;
    snd_alloc->ioa_Request.io_Flags = ADIOF_NOWAIT;
    snd_alloc->ioa_Data = chans;
    snd_alloc->ioa_Length = sizeof(chans);
    if (OpenDevice((STRPTR)AUDIONAME, 0, (struct IORequest *)snd_alloc, 0)) {
        DeleteIORequest((struct IORequest *)snd_alloc);
        snd_alloc = NULL;
        return FALSE;
    }
    units = (ULONG)snd_alloc->ioa_Request.io_Unit;
    snd_io[0] = *snd_alloc;
    snd_io[1] = *snd_alloc;
    snd_io[0].ioa_Request.io_Unit = (struct Unit *)(units & 9); /* channels 0, 3: left */
    snd_io[1].ioa_Request.io_Unit = (struct Unit *)(units & 6); /* 1, 2: right */
    return TRUE;
}

static void sound_play(float pan, float loud)
{
    int c;
    if (!snd_alloc)
        return;
    for (c = 0; c < 2; c++) {
        struct IOAudio *a = &snd_io[c];
        float v = loud * (c ? 0.5f + pan * 0.5f : 0.5f - pan * 0.5f);
        if (snd_busy[c]) {
            if (!CheckIO((struct IORequest *)a))
                AbortIO((struct IORequest *)a);
            WaitIO((struct IORequest *)a);
        }
        a->ioa_Request.io_Command = CMD_WRITE;
        a->ioa_Request.io_Flags = ADIOF_PERVOL;
        a->ioa_Data = (UBYTE *)snd;
        a->ioa_Length = SND_LEN;
        a->ioa_Period = 3546895 / SND_RATE;
        a->ioa_Volume = (UWORD)(64.0f * (v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v));
        a->ioa_Cycles = 1;
        begin_io((struct IORequest *)a);
        snd_busy[c] = TRUE;
    }
}

static void sound_close(void)
{
    int c;
    if (snd_alloc) {
        for (c = 0; c < 2; c++)
            if (snd_busy[c]) {
                if (!CheckIO((struct IORequest *)&snd_io[c]))
                    AbortIO((struct IORequest *)&snd_io[c]);
                WaitIO((struct IORequest *)&snd_io[c]);
            }
        CloseDevice((struct IORequest *)snd_alloc);
        DeleteIORequest((struct IORequest *)snd_alloc);
    }
    if (snd_port)
        DeleteMsgPort(snd_port);
    if (snd)
        FreeMem(snd, SND_LEN);
}

/* ---- input and time ---- */
static double now_s(void)
{
    struct TimeVal tv;
    GetSysTime((TimeVal_Type *)&tv);
    return tv.tv_secs + tv.tv_micro / 1e6;
}

enum { F_SHADOW, F_REFL, F_BLOOM, F_GOURAUD, F_AA, F_CLASSIC, F_COUNT };

struct input {
    BOOL quit, dragging, paused, timing, show_map, light_still;
    BOOL flag[F_COUNT];
    WORD last_x, last_y;
    float yaw, pitch;
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
            else if (m->Code >= '1' && m->Code < '1' + F_COUNT)
                in->flag[m->Code - '1'] = !in->flag[m->Code - '1'];
            else if (m->Code == ' ')
                in->paused = !in->paused;
            else if (m->Code == 't' || m->Code == 'T')
                in->timing = TRUE;
            else if (m->Code == 'm' || m->Code == 'M')
                in->show_map = !in->show_map;
            else if (m->Code == 'l' || m->Code == 'L')
                in->light_still = !in->light_still;
            break;
        case IDCMP_MOUSEBUTTONS:
            in->dragging = m->Code == SELECTDOWN;
            in->last_x = m->MouseX;
            in->last_y = m->MouseY;
            break;
        case IDCMP_MOUSEMOVE:
            if (in->dragging) {
                in->yaw += (m->MouseX - in->last_x) * 0.006f;
                in->pitch += (m->MouseY - in->last_y) * 0.004f;
                /* Inside the room. */
                if (in->yaw < -0.36f)
                    in->yaw = -0.36f;
                if (in->yaw > 0.36f)
                    in->yaw = 0.36f;
                if (in->pitch < -0.15f)
                    in->pitch = -0.15f;
                if (in->pitch > 0.36f)
                    in->pitch = 0.36f;
                in->last_x = m->MouseX;
                in->last_y = m->MouseY;
            }
            break;
        }
        ReplyMsg((struct Message *)m);
    }
}

/* A light's shadow map: orthographic, 11 units back from the ball along the
 * light's direction, so the shadow points away from where the light is. */
static mat4 light_view(vec3 light, vec3 at)
{
    vec3 dir = normalize(sub(at, light));
    vec3 up = dir.y > 0.9f || dir.y < -0.9f ? v3(0.0f, 0.0f, -1.0f) : v3(0.0f, 1.0f, 0.0f);
    return mul(ortho(SHADOW_HALF, 1.0f, 22.0f),
               look_at(v3(at.x - dir.x * 11.0f, at.y - dir.y * 11.0f, at.z - dir.z * 11.0f), at, up));
}

/* ---- the second light's lens flare ---- */
static vec3 LIGHT2, LIGHT2COL; /* defined with the lights, below */
struct flare_prog {
    GLuint p;
    GLint rect, col, kind;
};

/* Added to the window after the ball: the light itself where it is on the
 * screen, then a halo and ghosts along the line from it through the
 * screen's centre, as a camera lens makes them. Fades out when the ball is
 * between the light and the eye, or the light is behind the eye. */
static void draw_flare(struct flare_prog *f, const mat4 *vp, vec3 eye, vec3 ball, float aspect)
{
    static const struct { float at, size, kind, tint; } piece[] = {
        {1.0f, 0.42f, 0.0f, 0.0f},  /* the light */
        {1.0f, 0.30f, 1.0f, 0.0f},  /* its halo */
        {0.45f, 0.07f, 2.0f, 0.15f}, {0.15f, 0.12f, 2.0f, 0.35f}, {-0.25f, 0.05f, 2.0f, 0.5f},
        {-0.55f, 0.16f, 2.0f, 0.7f}, {-0.9f, 0.09f, 2.0f, 0.85f}, {-1.3f, 0.22f, 1.0f, 0.6f},
    };
    const float *m = vp->m;
    float cw = m[3] * LIGHT2.x + m[7] * LIGHT2.y + m[11] * LIGHT2.z + m[15];
    float cx, cy, vis, t, b;
    vec3 d, oc;
    int i;
    if (cw <= 0.1f)
        return;
    cx = (m[0] * LIGHT2.x + m[4] * LIGHT2.y + m[8] * LIGHT2.z + m[12]) / cw;
    cy = (m[1] * LIGHT2.x + m[5] * LIGHT2.y + m[9] * LIGHT2.z + m[13]) / cw;
    /* Hidden by the ball: the ray from the eye to the light against the
     * ball, softened over its edge. */
    d = sub(LIGHT2, eye);
    t = dot(sub(ball, eye), d) / dot(d, d);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    oc = sub(ball, v3(eye.x + d.x * t, eye.y + d.y * t, eye.z + d.z * t));
    b = ((float)sqrt(dot(oc, oc)) - BALL_R * 0.85f) / (BALL_R * 0.3f);
    vis = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
    /* Fades as the light leaves the screen. */
    b = 1.3f - ((float)fabs(cx) > (float)fabs(cy) ? (float)fabs(cx) : (float)fabs(cy));
    vis *= b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
    if (vis <= 0.01f)
        return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(f->p);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
    glDisableVertexAttribArray(1);
    for (i = 0; i < (int)(sizeof(piece) / sizeof(piece[0])); i++) {
        /* Ghosts are tinted towards the light's colour's neighbours. */
        float k = piece[i].tint, s = piece[i].size;
        vec3 c = LIGHT2COL;
        if (k > 0.0f)
            c = v3(c.y * k + c.x * (1.0f - k), c.z * k + c.y * (1.0f - k), c.x * k + c.z * (1.0f - k));
        glUniform4f(f->rect, cx * piece[i].at, cy * piece[i].at, s / aspect, s);
        glUniform3f(f->col, c.x * 0.4f * vis, c.y * 0.4f * vis, c.z * 0.4f * vis);
        glUniform1f(f->kind, piece[i].kind);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glDisable(GL_BLEND);
}

/* ---- TOUR: what each effect proves, one at a time ---- */
struct tour_stop {
    const char *say;
    BOOL flag[F_COUNT]; /* shadow, reflection, bloom, Gouraud, AA, classic */
    BOOL map;
};

static const struct tour_stop tour_stops[] = {
    {"Boing 2026: every effect on", {1, 1, 1, 0, 1, 0}, 0},
    {"1984: flat colours, a flat shadow", {1, 1, 1, 0, 1, 1}, 0},
    {"per-vertex (Gouraud) light: the highlight smears", {1, 1, 1, 1, 1, 0}, 0},
    {"per-pixel GGX light: a sharp highlight", {1, 1, 1, 0, 1, 0}, 0},
    {"checker without fwidth: jagged, crawling edges", {1, 1, 1, 0, 0, 0}, 0},
    {"no shadow map", {0, 1, 1, 0, 1, 0}, 0},
    {"shadow map: a depth texture, 12 PCF taps", {1, 1, 1, 0, 1, 0}, 1},
    {"no reflection", {1, 0, 1, 0, 1, 0}, 0},
    {"no bloom", {1, 1, 0, 0, 1, 0}, 0},
};
#define TOUR_STOPS (int)(sizeof(tour_stops) / sizeof(tour_stops[0]))
#define TOUR_SECONDS 5.0

/* ---- programs and their uniforms ---- */
struct ball_prog {
    GLuint p;
    GLint model, vp, nmat, light, lightcol, light2, light2col, eye, mode, post;
};
struct room_prog {
    GLuint p;
    GLint vp, lightvp[2], light, lightcol, light2, light2col, eye, ball, mode, inv_screen, soft[2], occluder[2],
        backz, ceil;
};

/* The light circles the ball, high up (frame by frame in main). */
static vec3 LIGHT = {-0.6f, 6.6f, 1.2f};
static const vec3 LIGHTCOL = {5.2f, 4.8f, 4.4f};
/* The second light: its own shadow, a path of its own, and a colour that
 * runs round the colour wheel (set each frame by light2_colour). */
static vec3 LIGHT2 = {2.0f, 3.0f, 2.0f};
static vec3 LIGHT2COL = {0.25f, 1.9f, 2.6f};

/* A fully saturated hue (0..1 round the wheel), at the second light's
 * strength. */
static void light2_colour(float hue)
{
    float h = (hue - (float)floor(hue)) * 6.0f, c[3];
    int i;
    c[0] = (float)fabs(h - 3.0f) - 1.0f;
    c[1] = 2.0f - (float)fabs(h - 2.0f);
    c[2] = 2.0f - (float)fabs(h - 4.0f);
    for (i = 0; i < 3; i++)
        c[i] = (c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i]) * 0.9f + 0.1f;
    LIGHT2COL = v3(c[0] * 2.6f, c[1] * 2.6f, c[2] * 2.6f);
}

/* post: for the window (out 0), 1 / its size, the bloom's strength and its
 * aspect; the bloom is on texture unit 2. */
static float post[4];

static void draw_ball(struct ball_prog *b, const struct scene *s, const mat4 *vp, vec3 eye, float out,
                      const struct input *in)
{
    float n3[9];
    glUseProgram(b->p);
    upper3(&s->nrm, n3);
    glUniformMatrix4fv(b->model, 1, GL_FALSE, s->model.m);
    glUniformMatrix4fv(b->vp, 1, GL_FALSE, vp->m);
    glUniformMatrix3fv(b->nmat, 1, GL_FALSE, n3);
    glUniform3f(b->light, LIGHT.x, LIGHT.y, LIGHT.z);
    glUniform3f(b->lightcol, LIGHTCOL.x, LIGHTCOL.y, LIGHTCOL.z);
    glUniform3f(b->light2, LIGHT2.x, LIGHT2.y, LIGHT2.z);
    glUniform3f(b->light2col, LIGHT2COL.x, LIGHT2COL.y, LIGHT2COL.z);
    glUniform3f(b->eye, eye.x, eye.y, eye.z);
    glUniform4f(b->mode, in->flag[F_GOURAUD] ? 1.0f : 0.0f, in->flag[F_AA] ? 1.0f : 0.0f,
                in->flag[F_CLASSIC] ? 1.0f : 0.0f, out);
    glUniform4f(b->post, post[0], post[1], post[2], post[3]);
    glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (const void *)0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphere_ibo);
    glDrawElements(GL_TRIANGLES, sphere_count, GL_UNSIGNED_SHORT, (const void *)0);
}

static void draw_room(struct room_prog *r, const struct scene *s, const mat4 *vp, const mat4 *lightvp, vec3 eye,
                      float out, int w, int h, const struct input *in, BOOL refl_ready)
{
    float soft = 0.004f + 0.026f * (s->centre.y - BALL_R) / PEAK;
    int k;
    if (soft < 0.004f)
        soft = 0.004f;
    glUseProgram(r->p);
    /* Each light's shadow: the ball's footprint in its map (an orthographic
     * one: the same for every receiver), with its bulge when squashed and
     * the PCF radius. */
    for (k = 0; k < 2; k++) {
        const float *m = lightvp[k].m;
        float px = m[0] * s->centre.x + m[4] * s->centre.y + m[8] * s->centre.z + m[12];
        float py = m[1] * s->centre.x + m[5] * s->centre.y + m[9] * s->centre.z + m[13];
        float rad = 0.5f * BALL_R * 1.3f / SHADOW_HALF + soft + 0.01f;
        glUniformMatrix4fv(r->lightvp[k], 1, GL_FALSE, m);
        glUniform1f(r->soft[k], in->flag[F_CLASSIC] ? 0.0f : soft);
        glUniform3f(r->occluder[k], px * 0.5f + 0.5f, py * 0.5f + 0.5f, rad);
    }
    glUniformMatrix4fv(r->vp, 1, GL_FALSE, vp->m);
    glUniform3f(r->light, LIGHT.x, LIGHT.y, LIGHT.z);
    glUniform3f(r->lightcol, LIGHTCOL.x, LIGHTCOL.y, LIGHTCOL.z);
    glUniform3f(r->light2, LIGHT2.x, LIGHT2.y, LIGHT2.z);
    glUniform3f(r->light2col, LIGHT2COL.x, LIGHT2COL.y, LIGHT2COL.z);
    glUniform3f(r->eye, eye.x, eye.y, eye.z);
    glUniform4f(r->ball, s->centre.x, s->centre.y, s->centre.z, BALL_R);
    glUniform4f(r->mode, in->flag[F_SHADOW] || in->flag[F_CLASSIC] ? 1.0f : 0.0f, refl_ready ? 1.0f : 0.0f,
                in->flag[F_CLASSIC] ? 1.0f : 0.0f, out);
    glUniform2f(r->inv_screen, 1.0f / w, 1.0f / h);
    glUniform1f(r->backz, ROOM_BACK);
    glUniform1f(r->ceil, ROOM_H);
    glBindBuffer(GL_ARRAY_BUFFER, room_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (const void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (const void *)12);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, 30);
}

static void draw_screen_tri(void)
{
    glBindBuffer(GL_ARRAY_BUFFER, tri_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
    glDisableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* With T: finishes the GPU's work and returns the milliseconds since the
 * last mark. */
static double mark_t;
static double mark(BOOL on)
{
    double n, d;
    if (!on)
        return 0.0;
    glFinish();
    n = now_s();
    d = (n - mark_t) * 1000.0;
    mark_t = n;
    return d;
}

int main(void)
{
    static const GLfloat tri[] = {-1, -1, 3, -1, -1, 3};
    static char title[120];
    LONG args[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    struct RDArgs *rda;
    struct Window *win = NULL, *iw = NULL;
    void *ctx = NULL;
    BOOL full, msaa, sound, auto_timing, vsync, tour;
    int tour_step = -1;
    ULONG err = 0, width = 0, height = 0;
    struct ball_prog bp;
    struct room_prog rp;
    struct flare_prog fp;
    GLuint blur = 0, comp = 0, shad = 0;
    GLint ub_dir, uc_bloom, uc_aspect, uc_vig, us_mvp;
    struct rt refl = {0, 0, 0, 0}, bloom_a = {0, 0, 0, 0}, bloom_b = {0, 0, 0, 0}, scene_rt = {0, 0, 0, 0};
    struct input in;
    struct scene sc;
    double start, last, last_title;
    unsigned long frames = 0, title_frames = 0, max_frames = 0;
    int w0 = 640, h0 = 480, rc = 20, i, scale = 0, sw, sh;
    float light_t = 3.0f;
    BOOL timer_open = FALSE;
    const char *ext;

    memset(&in, 0, sizeof(in));
    in.flag[F_SHADOW] = in.flag[F_REFL] = in.flag[F_BLOOM] = in.flag[F_AA] = TRUE;
    in.pitch = 0.10f;
    memset(&sc, 0, sizeof(sc));
    sc.vx = SPEED_X;
    sc.phase = 0.5f;
    sc.sx = sc.sy = 1.0f;
    memset(&bp, 0, sizeof(bp));
    memset(&rp, 0, sizeof(rp));
    memset(&fp, 0, sizeof(fp));

    rda = ReadArgs((STRPTR) "WIDTH/N,HEIGHT/N,FRAMES/N,NOMSAA/S,NOSOUND/S,FULLSCREEN/S,TIMING/S,SHOWMAP/S,VSYNC/S,TOUR/S,SCALE/N", args, NULL);
    if (!rda) {
        printf("usage: boing2026 [WIDTH=w] [HEIGHT=h] [FRAMES=n] [NOMSAA] [NOSOUND] [FULLSCREEN] [TIMING] [SHOWMAP] "
               "[VSYNC] [TOUR] [SCALE=percent]\n");
        return 20;
    }
    if (args[0])
        w0 = *(LONG *)args[0];
    if (args[1])
        h0 = *(LONG *)args[1];
    if (args[2])
        max_frames = *(LONG *)args[2];
    msaa = !args[3];
    sound = !args[4];
    full = args[5] != 0;
    auto_timing = args[6] != 0;
    in.show_map = args[7] != 0;
    vsync = args[8] != 0;
    tour = args[9] != 0;
    if (args[10])
        scale = *(LONG *)args[10];
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
        printf("needs %s 1.3 or newer: in LIBS:, or in this program's drawer or its libs/\n", OGLES2NAME);
        goto out;
    }
    if (full) {
        /* The first 32-bit mode of the size asked for that ogles2 accepts. */
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
                                        OGLES2_CCT_DEPTH, 0, OGLES2_CCT_STENCIL, 0, OGLES2_CCT_PISTORM_MSAA,
                                        msaa ? 4 : 0, OGLES2_CCT_VSYNC, vsync, OGLES2_CCT_GET_WIDTH, (ULONG)&width,
                                        OGLES2_CCT_GET_HEIGHT, (ULONG)&height, TAG_DONE);
        }
        if (!ctx || !iw) {
            printf("no %dx%d screen mode that ogles2 can draw into (it needs a 32-bit one)\n", w0, h0);
            goto out;
        }
        ModifyIDCMP(iw, IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE);
        iw->Flags |= WFLG_REPORTMOUSE | WFLG_RMBTRAP;
    } else {
        win = OpenWindowTags(NULL, WA_Title, (ULONG) "Boing 2026", WA_InnerWidth, w0, WA_InnerHeight, h0,
                             WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
                             WA_ReportMouse, TRUE, WA_RMBTrap, TRUE,
                             WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE,
                             TAG_DONE);
        if (!win) {
            printf("cannot open a window\n");
            goto out;
        }
        ctx = aglCreateContextTags2(&err, OGLES2_CCT_WINDOW, (ULONG)win, OGLES2_CCT_DEPTH, 0, OGLES2_CCT_STENCIL, 0,
                                    OGLES2_CCT_PISTORM_MSAA, msaa ? 4 : 0, OGLES2_CCT_VSYNC, vsync,
                                    OGLES2_CCT_GET_WIDTH, (ULONG)&width, OGLES2_CCT_GET_HEIGHT, (ULONG)&height,
                                    TAG_DONE);
        if (!ctx) {
            printf("aglCreateContextTags2 failed (error %lu)\n", (unsigned long)err);
            goto out;
        }
        iw = win;
    }
    aglMakeCurrent(ctx);

    /* What the effects need, said once. */
    ext = (const char *)glGetString(GL_EXTENSIONS);
    printf("Boing 2026 on %s, %lux%lu, MSAA %s\n", (const char *)glGetString(GL_RENDERER), (unsigned long)width,
           (unsigned long)height, msaa ? "4x" : "off");
    {
        static const char *need[] = {"OES_depth_texture", "OES_standard_derivatives", "EXT_discard_framebuffer"};
        for (i = 0; i < 3; i++)
            if (!ext || !strstr(ext, need[i])) {
                printf("this needs %s\n", need[i]);
                goto out;
            }
    }

    if (!(bp.p = link_program(ball_vert, ball_frag)) || !(rp.p = link_program(room_vert, room_frag)) ||
        !(blur = link_program(screen_vert, blur_frag)) || !(comp = link_program(screen_vert, composite_frag)) ||
        !(shad = link_program(shadow_vert, shadow_frag)) || !(fp.p = link_program(flare_vert, flare_frag)))
        goto out;
    fp.rect = glGetUniformLocation(fp.p, "u_rect");
    fp.col = glGetUniformLocation(fp.p, "u_col");
    fp.kind = glGetUniformLocation(fp.p, "u_kind");
    bp.model = glGetUniformLocation(bp.p, "u_model");
    bp.vp = glGetUniformLocation(bp.p, "u_vp");
    bp.nmat = glGetUniformLocation(bp.p, "u_nmat");
    bp.light = glGetUniformLocation(bp.p, "u_light");
    bp.lightcol = glGetUniformLocation(bp.p, "u_lightcol");
    bp.light2 = glGetUniformLocation(bp.p, "u_light2");
    bp.light2col = glGetUniformLocation(bp.p, "u_light2col");
    bp.eye = glGetUniformLocation(bp.p, "u_eye");
    bp.mode = glGetUniformLocation(bp.p, "u_mode");
    bp.post = glGetUniformLocation(bp.p, "u_post");
    glUseProgram(bp.p);
    glUniform1i(glGetUniformLocation(bp.p, "u_bloomtex"), 2);
    rp.vp = glGetUniformLocation(rp.p, "u_vp");
    rp.lightvp[0] = glGetUniformLocation(rp.p, "u_lightvp");
    rp.lightvp[1] = glGetUniformLocation(rp.p, "u_lightvp2");
    rp.light = glGetUniformLocation(rp.p, "u_light");
    rp.lightcol = glGetUniformLocation(rp.p, "u_lightcol");
    rp.light2 = glGetUniformLocation(rp.p, "u_light2");
    rp.light2col = glGetUniformLocation(rp.p, "u_light2col");
    rp.eye = glGetUniformLocation(rp.p, "u_eye");
    rp.ball = glGetUniformLocation(rp.p, "u_ball");
    rp.mode = glGetUniformLocation(rp.p, "u_mode");
    rp.inv_screen = glGetUniformLocation(rp.p, "u_inv_screen");
    rp.soft[0] = glGetUniformLocation(rp.p, "u_soft");
    rp.soft[1] = glGetUniformLocation(rp.p, "u_soft2");
    rp.backz = glGetUniformLocation(rp.p, "u_backz");
    rp.occluder[0] = glGetUniformLocation(rp.p, "u_occluder");
    rp.occluder[1] = glGetUniformLocation(rp.p, "u_occluder2");
    rp.ceil = glGetUniformLocation(rp.p, "u_ceil");
    glUseProgram(rp.p);
    glUniform1i(glGetUniformLocation(rp.p, "u_shadow"), 0);
    glUniform1i(glGetUniformLocation(rp.p, "u_shadow2"), 3);
    glUniform1i(glGetUniformLocation(rp.p, "u_refl"), 1);
    glUseProgram(blur);
    glUniform1i(glGetUniformLocation(blur, "u_tex"), 0);
    ub_dir = glGetUniformLocation(blur, "u_dir");
    glUseProgram(comp);
    glUniform1i(glGetUniformLocation(comp, "u_scene"), 0);
    glUniform1i(glGetUniformLocation(comp, "u_tex"), 2);
    uc_bloom = glGetUniformLocation(comp, "u_bloom");
    uc_aspect = glGetUniformLocation(comp, "u_aspect");
    uc_vig = glGetUniformLocation(comp, "u_vig");
    us_mvp = glGetUniformLocation(shad, "u_mvp");

    make_sphere();
    make_room();
    glGenBuffers(1, &tri_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, tri_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    {
        static const GLfloat quad[] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
        glGenBuffers(1, &quad_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    }
    glEnableVertexAttribArray(0);
    /* The render scale: the room is drawn at that fraction of the window
     * and brought up to it; the ball always at the window's size, with
     * MSAA. By default 78% of 640 x 480's worth of pixels: 50% at
     * 1280 x 720. */
    if (scale <= 0) {
        double s = 100.0 * sqrt(640.0 * 480.0 * 0.78 / ((double)width * height));
        scale = s >= 100.0 || (ULONG)width * height <= 640 * 480 ? 100 : (int)s;
    }
    if (scale < 25)
        scale = 25;
    if (scale > 100)
        scale = 100;
    sw = width * scale / 100;
    sh = height * scale / 100;
    printf("render scale %d%%: the room at %dx%d\n", scale, sw, sh);
    if (!make_shadow(0) || !make_shadow(1) || !make_rt(&scene_rt, sw, sh) || !make_rt(&refl, sw / 2, sh / 2) ||
        !make_rt(&bloom_a, sw / 4, sh / 4) || !make_rt(&bloom_b, sw / 4, sh / 4))
        goto out;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    if (sound && !sound_open()) {
        printf("no audio channels: playing without the boing\n");
        sound_close();
        snd = NULL, snd_port = NULL, snd_alloc = NULL;
    }

    start = last = last_title = now_s();
    while (!in.quit) {
        double n = now_s(), ms[6];
        float dt = (float)(n - last), aspect = (float)width / (float)height;
        BOOL timing = in.timing || (auto_timing && frames % 120 == 60), refl_ready;
        vec3 eye, target = v3(0.0f, 1.9f, 0.0f), meye;
        mat4 proj, view, vp, mvp, lightvp[2];
        last = n;
        events(iw, &in);
        in.timing = FALSE;
        if (tour && (int)((n - start) / TOUR_SECONDS) % TOUR_STOPS != tour_step) {
            const struct tour_stop *ts;
            tour_step = (int)((n - start) / TOUR_SECONDS) % TOUR_STOPS;
            ts = &tour_stops[tour_step];
            for (i = 0; i < F_COUNT; i++)
                in.flag[i] = ts->flag[i];
            in.show_map = ts->map;
            printf("tour %d: %s\n", tour_step + 1, ts->say);
            last_title = 0.0; /* the title at once */
        }
        if (dt > 0.05f)
            dt = 0.05f;
        if (!in.paused)
            scene_step(&sc, dt);
        if (sc.hit_floor)
            sound_play(sc.x / ROOM_X, 0.9f);
        if (sc.hit_wall)
            sound_play(sc.x > 0.0f ? 1.0f : -1.0f, 0.55f);

        /* The camera breathes slowly around the room; the mouse adds to it. */
        {
            float t = (float)(n - start), yaw = in.yaw + 0.16f * (float)sin(t * 0.13f);
            float pitch = in.pitch + 0.04f * (float)sin(t * 0.21f), d = 9.6f;
            eye = v3((float)sin(yaw) * (float)cos(pitch) * d, target.y + (float)sin(pitch) * d,
                     (float)cos(yaw) * (float)cos(pitch) * d);
            if (eye.y < 0.4f)
                eye.y = 0.4f;
        }
        proj = perspective(48.0f * PI / 180.0f, aspect, 0.2f, 40.0f);
        view = look_at(eye, target, v3(0.0f, 1.0f, 0.0f));
        vp = mul(proj, view);
        /* The light flies round the ball: sines of unrelated periods, so the
         * path never quite repeats. It speeds up and slows down round the
         * ball, swings in and out (2 to 4 units), and climbs and dives
         * between under the ceiling and the ball's own height. Its panel
         * on the ceiling follows it. The shadow map looks from it at the
         * ball, so the shadow swings and stretches round the ball. */
        if (!in.light_still && !in.paused)
            light_t += dt;
        {
            vec3 at = v3(sc.centre.x, sc.centre.y * 0.5f, 0.0f);
            float t = light_t;
            float a = t * 0.7f + 0.9f * (float)sin(t * 0.31f);
            float r = 3.0f + 1.0f * (float)sin(t * 0.47f + 1.0f);
            float y = 4.6f + 1.6f * (float)sin(t * 0.83f) + 0.6f * (float)sin(t * 1.9f + 0.4f);
            LIGHT = v3(at.x + r * (float)cos(a), y > 6.7f ? 6.7f : y, r * (float)sin(a) * 0.8f + 0.5f);
            if (LIGHT.x > ROOM_X - 1.6f)
                LIGHT.x = ROOM_X - 1.6f;
            if (LIGHT.x < -ROOM_X + 1.6f)
                LIGHT.x = -ROOM_X + 1.6f;
            /* The second light: the other way round, lower, faster up
             * and down; it may pass under the ball and light it from
             * below. */
            a = -t * 0.9f + 1.3f * (float)sin(t * 0.27f + 2.0f) + 2.5f;
            r = 2.4f + 0.8f * (float)sin(t * 0.61f);
            y = 2.4f + 1.5f * (float)sin(t * 1.13f + 1.0f) + 0.5f * (float)sin(t * 2.3f);
            LIGHT2 = v3(at.x + r * (float)cos(a), y < 0.3f ? 0.3f : y, r * (float)sin(a) * 0.9f + 0.6f);
            if (LIGHT2.x > ROOM_X - 0.5f)
                LIGHT2.x = ROOM_X - 0.5f;
            if (LIGHT2.x < -ROOM_X + 0.5f)
                LIGHT2.x = -ROOM_X + 0.5f;
            light2_colour(t * 0.06f);
            lightvp[0] = light_view(LIGHT, at);
            lightvp[1] = light_view(LIGHT2, at);
        }
        mark(timing);

        /* 1: the shadow maps, one a light (the 1984 look: the first only). */
        if (in.flag[F_SHADOW] || in.flag[F_CLASSIC]) {
            int k;
            glEnable(GL_DEPTH_TEST);
            glUseProgram(shad);
            glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (const void *)0);
            glDisableVertexAttribArray(1);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphere_ibo);
            for (k = 0; k < 2; k++) {
                glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo[k]);
                glViewport(0, 0, SHADOW_RES, SHADOW_RES);
                glClear(GL_DEPTH_BUFFER_BIT | (shadow_rb[k] ? GL_COLOR_BUFFER_BIT : 0));
                mvp = mul(lightvp[k], sc.model);
                glUniformMatrix4fv(us_mvp, 1, GL_FALSE, mvp.m);
                glDrawElements(GL_TRIANGLES, sphere_count, GL_UNSIGNED_SHORT, (const void *)0);
                if (shadow_rb[k])
                    discard(1, GL_COLOR_ATTACHMENT0, 0);
            }
            glDisable(GL_DEPTH_TEST);
        }
        ms[0] = mark(timing);

        /* 2: the ball upside down under the floor, for its reflection. The
         * mirror turns the triangles round. */
        refl_ready = in.flag[F_REFL] && !in.flag[F_CLASSIC];
        if (refl_ready) {
            mat4 mvpr = mul(vp, scale3(1.0f, -1.0f, 1.0f));
            glBindFramebuffer(GL_FRAMEBUFFER, refl.fbo);
            glViewport(0, 0, refl.w, refl.h);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glFrontFace(GL_CW);
            meye = v3(eye.x, -eye.y, eye.z);
            draw_ball(&bp, &sc, &mvpr, meye, 2.0f, &in);
            glFrontFace(GL_CCW);
        }
        ms[1] = mark(timing);

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, shadow_tex[1]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, refl.tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, shadow_tex[0]);

        /* 3: what glows, at 1/4 size, blurred twice each way. */
        if (in.flag[F_BLOOM] && !in.flag[F_CLASSIC]) {
            int k;
            glBindFramebuffer(GL_FRAMEBUFFER, bloom_a.fbo);
            glViewport(0, 0, bloom_a.w, bloom_a.h);
            glClear(GL_COLOR_BUFFER_BIT); /* else the tiles are loaded first */
            draw_room(&rp, &sc, &vp, lightvp, eye, 1.0f, bloom_a.w, bloom_a.h, &in, refl_ready);
            draw_ball(&bp, &sc, &vp, eye, 1.0f, &in);
            glUseProgram(blur);
            for (k = 0; k < 2; k++) {
                float spread = k ? 2.2f : 1.0f;
                glBindFramebuffer(GL_FRAMEBUFFER, bloom_b.fbo);
                glClear(GL_COLOR_BUFFER_BIT);
                glBindTexture(GL_TEXTURE_2D, bloom_a.tex);
                glUniform2f(ub_dir, spread / bloom_a.w, 0.0f);
                draw_screen_tri();
                glBindFramebuffer(GL_FRAMEBUFFER, bloom_a.fbo);
                glClear(GL_COLOR_BUFFER_BIT);
                glBindTexture(GL_TEXTURE_2D, bloom_b.tex);
                glUniform2f(ub_dir, 0.0f, spread / bloom_a.h);
                draw_screen_tri();
            }
            glBindTexture(GL_TEXTURE_2D, shadow_tex[0]);
        }
        ms[2] = mark(timing);

        /* 4: the room, at the render scale. */
        glBindFramebuffer(GL_FRAMEBUFFER, scene_rt.fbo);
        glViewport(0, 0, sw, sh);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_room(&rp, &sc, &vp, lightvp, eye, in.show_map ? 3.0f : 0.0f, sw, sh, &in, refl_ready);
        ms[3] = mark(timing);

        /* 5: into the window: the room brought up to its size with the bloom
         * and the vignette, one opaque pass; then the ball over it at full
         * size and antialiased, adding its own bloom and vignette. */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        glClear(GL_COLOR_BUFFER_BIT);
        {
            BOOL bloom_on = in.flag[F_BLOOM] && !in.flag[F_CLASSIC];
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, bloom_a.tex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, scene_rt.tex);
            glUseProgram(comp);
            glUniform1f(uc_bloom, bloom_on ? BLOOM : 0.0f);
            glUniform1f(uc_aspect, aspect);
            glUniform1f(uc_vig, in.flag[F_CLASSIC] ? 0.0f : 1.0f);
            draw_screen_tri();
            post[0] = 1.0f / width, post[1] = 1.0f / height;
            post[2] = bloom_on ? BLOOM : 0.0f, post[3] = aspect;
            draw_ball(&bp, &sc, &vp, eye, 0.0f, &in);
            if (!in.flag[F_CLASSIC])
                draw_flare(&fp, &vp, eye, sc.centre, aspect);
            /* Unit 2 unbound: the bloom pass draws into that texture. */
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);
        }
        /* SHOWMAP: the shadow map in the corner, through the blur
         * program with no blur. */
        if (in.show_map) {
            glViewport(0, 0, 160, 160);
            glDisable(GL_BLEND);
            glUseProgram(blur);
            glBindTexture(GL_TEXTURE_2D, shadow_tex[0]);
            glUniform2f(ub_dir, 0.0f, 0.0f);
            draw_screen_tri();
        }
        ms[4] = mark(timing);
        aglSwapBuffers();
        ms[5] = mark(timing);
        if (timing)
            printf("ms: shadow %.2f  reflection %.2f  bloom %.2f  scene %.2f  composite %.2f  swap %.2f\n", ms[0],
                   ms[1], ms[2], ms[3], ms[4], ms[5]);

        frames++;
        title_frames++;
        if (!full && tour && n - last_title >= 1.0) {
            sprintf(title, "Boing 2026  %.1f fps  %d/%d: %s", last_title > 0.0 ? title_frames / (n - last_title) : 0.0,
                    tour_step + 1, TOUR_STOPS, tour_stops[tour_step].say);
            SetWindowTitles(win, (UBYTE *)title, (UBYTE *)-1);
            last_title = n;
            title_frames = 0;
        } else if (!full && n - last_title >= 1.0) {
            sprintf(title, "Boing 2026  %.1f fps  [1]shadow:%s [2]refl:%s [3]bloom:%s [4]%s [5]aa:%s [6]%s",
                    title_frames / (n - last_title), in.flag[F_SHADOW] ? "on" : "off",
                    in.flag[F_REFL] ? "on" : "off", in.flag[F_BLOOM] ? "on" : "off",
                    in.flag[F_GOURAUD] ? "gouraud" : "per-pixel", in.flag[F_AA] ? "on" : "off",
                    in.flag[F_CLASSIC] ? "1984" : "2026");
            SetWindowTitles(win, (UBYTE *)title, (UBYTE *)-1);
            last_title = n;
            title_frames = 0;
        }
        if (max_frames && frames >= max_frames)
            break;
    }
    if (frames) {
        double s = now_s() - start;
        printf("%lu frames in %.1f s: %.2f fps at %lux%lu\n", frames, s, frames / s, (unsigned long)width,
               (unsigned long)height);
    }
    rc = 0;
out:
    sound_close();
    if (ctx) {
        free_rt(&refl);
        free_rt(&scene_rt);
        free_rt(&bloom_a);
        free_rt(&bloom_b);
        for (i = 0; i < 2; i++) {
            if (shadow_fbo[i])
                glDeleteFramebuffers(1, &shadow_fbo[i]);
            if (shadow_tex[i])
                glDeleteTextures(1, &shadow_tex[i]);
            if (shadow_rb[i])
                glDeleteRenderbuffers(1, &shadow_rb[i]);
        }
        if (sphere_vbo)
            glDeleteBuffers(1, &sphere_vbo);
        if (sphere_ibo)
            glDeleteBuffers(1, &sphere_ibo);
        if (room_vbo)
            glDeleteBuffers(1, &room_vbo);
        if (tri_vbo)
            glDeleteBuffers(1, &tri_vbo);
        if (quad_vbo)
            glDeleteBuffers(1, &quad_vbo);
        if (fp.p)
            glDeleteProgram(fp.p);
        if (bp.p)
            glDeleteProgram(bp.p);
        if (rp.p)
            glDeleteProgram(rp.p);
        if (blur)
            glDeleteProgram(blur);
        if (comp)
            glDeleteProgram(comp);
        if (shad)
            glDeleteProgram(shad);
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
