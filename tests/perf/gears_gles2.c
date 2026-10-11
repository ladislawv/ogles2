/*
 * The gears speed comparison, ogles2.library side (perf_gears.h).
 *   usage: gears_gles2 [BITMAP|WINDOW|FULLSCREEN] [SECONDS=5] [TEETH=1] [PATH=VBO|ARRAYS]
 * VBO: each gear's vertices in a buffer object, uploaded once; ARRAYS: from
 * client memory, converted for the GPU every draw. One glDrawArrays per gear,
 * the lighting in the vertex shader (tests/common/lit_shaders.h).
 */
#include <proto/ogles2.h>
#include "perf_target.h"
#include "../common/lit_shaders.h"

/* Column-major 4x4 matrices. */
static void mat_mul(float *r, const float *a, const float *b)
{
    float t[16];
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            t[j * 4 + i] = a[i] * b[j * 4] + a[4 + i] * b[j * 4 + 1] + a[8 + i] * b[j * 4 + 2] +
                           a[12 + i] * b[j * 4 + 3];
    memcpy(r, t, sizeof(t));
}

static void mat_identity(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_translate(float *m, float x, float y, float z)
{
    float t[16];
    mat_identity(t);
    t[12] = x;
    t[13] = y;
    t[14] = z;
    mat_mul(m, m, t);
}

/* As glRotatef about a unit axis. */
static void mat_rotate(float *m, float deg, float x, float y, float z)
{
    float t[16], a = deg * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a), k = 1.0f - c;
    mat_identity(t);
    t[0] = x * x * k + c;     t[4] = x * y * k - z * s; t[8] = x * z * k + y * s;
    t[1] = y * x * k + z * s; t[5] = y * y * k + c;     t[9] = y * z * k - x * s;
    t[2] = x * z * k - y * s; t[6] = y * z * k + x * s; t[10] = z * z * k + c;
    mat_mul(m, m, t);
}

static void mat_frustum(float *m, float l, float r, float b, float t, float n, float f)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2 * n / (r - l);
    m[5] = 2 * n / (t - b);
    m[8] = (r + l) / (r - l);
    m[9] = (t + b) / (t - b);
    m[10] = -(f + n) / (f - n);
    m[11] = -1;
    m[14] = -2 * f * n / (f - n);
}

static GLuint link(void)
{
    const char *vs = lit_vs, *fs = lit_fs;
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p;
    GLint ok = 0;
    glShaderSource(v, 1, &vs, 0);
    glShaderSource(f, 1, &fs, 0);
    glCompileShader(v);
    glCompileShader(f);
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_position");
    glBindAttribLocation(p, 1, "a_normal");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

static GLint u_mvp, u_mv, u_color;
static GLuint vbo[3];
static float proj[16];
static int vbo_path;

static void frame(float angle)
{
    float mv[16], mvp[16];
    int k;
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (k = 0; k < 3; k++) {
        float tx, ty, rz;
        pg_place(k, angle, &tx, &ty, &rz);
        mat_identity(mv);
        mat_translate(mv, 0, 0, -40);
        mat_rotate(mv, PG_VIEW_ROTX, 1, 0, 0);
        mat_rotate(mv, PG_VIEW_ROTY, 0, 1, 0);
        mat_translate(mv, tx, ty, 0);
        mat_rotate(mv, rz, 0, 0, 1);
        mat_mul(mvp, proj, mv);
        glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
        glUniformMatrix4fv(u_mv, 1, GL_FALSE, mv);
        glUniform4fv(u_color, 1, pg_gears[k].color);
        if (vbo_path) {
            glBindBuffer(GL_ARRAY_BUFFER, vbo[k]);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *)0);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12);
        } else {
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, pg_gears[k].v);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, pg_gears[k].v + 3);
        }
        glDrawArrays(GL_TRIANGLES, 0, pg_gears[k].count);
    }
}

int main(int argc, char **argv)
{
    struct pg_args a;
    ULONG err = 0, frames = 0, t0, t1;
    GLuint prog;
    float angle = 0.0f;
    int k, ok = 0;
    void *ctx = 0;
    char title[80];

    pg_parse(argc, argv, &a, "VBO");
    vbo_path = !pg_eq(a.path, "ARRAYS");
    a.path = vbo_path ? "VBO" : "ARRAYS";
    if (!pg_scene(a.mult)) {
        printf("out of memory\n");
        return 20;
    }
    sprintf(title, "ogles2 %s: %d triangles", a.path, pg_triangles());
    if (!pt_open(&a, title) || !OGLES2Open()) {
        printf("needs a window or bitmap and %s\n", OGLES2NAME);
        pt_close();
        return 20;
    }
    if (a.fullscreen) {
        ULONG mode = pt_find_mode();
        ctx = aglCreateContextTags2(&err, OGLES2_CCT_MODEID, mode, OGLES2_CCT_DEPTH, 24, OGLES2_CCT_VSYNC,
                                    FALSE, TAG_DONE);
    } else {
        ctx = aglCreateContextTags2(&err, a.window ? OGLES2_CCT_WINDOW : OGLES2_CCT_BITMAP,
                                    a.window ? (ULONG)pt_win : (ULONG)pt_bm, OGLES2_CCT_DEPTH, 24,
                                    OGLES2_CCT_VSYNC, FALSE, TAG_DONE);
    }
    if (!ctx) {
        printf("aglCreateContext2 failed (%lu)\n", (unsigned long)err);
        goto out;
    }
    aglMakeCurrent(ctx);
    if (!(prog = link())) {
        printf("the lit program does not link (its .v3dp next to the program?)\n");
        goto out;
    }
    glUseProgram(prog);
    u_mvp = glGetUniformLocation(prog, "u_mvp");
    u_mv = glGetUniformLocation(prog, "u_mv");
    u_color = glGetUniformLocation(prog, "u_color");
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    if (vbo_path) {
        glGenBuffers(3, vbo);
        for (k = 0; k < 3; k++) {
            glBindBuffer(GL_ARRAY_BUFFER, vbo[k]);
            glBufferData(GL_ARRAY_BUFFER, pg_gears[k].count * 6 * sizeof(float), pg_gears[k].v, GL_STATIC_DRAW);
        }
    }
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);
    mat_frustum(proj, -1, 1, -(float)PG_H / PG_W, (float)PG_H / PG_W, 5, 60);

    t0 = 0;
    while (!pt_break()) {
        if (frames == 5) /* the first frames load the program: not timed */
            t0 = pt_ticks();
        frame(angle);
        aglSwapBuffers();
        angle += PG_ANGLE_STEP;
        frames++;
        if (frames > 5 && pt_ticks() - t0 >= (ULONG)a.seconds * 50)
            break;
    }
    glFinish();
    t1 = pt_ticks();
    if (frames > 5) {
        pt_report("ogles2", &a, frames - 5, t1 - t0);
        frame(0.0f);
        aglSwapBuffers();
        pt_image("ogles2");
        ok = glGetError() == GL_NO_ERROR;
        if (!ok)
            printf("GL error\n");
    }
    if (vbo_path)
        glDeleteBuffers(3, vbo);
    glDeleteProgram(prog);
out:
    if (ctx)
        aglDestroyContext(ctx);
    OGLES2Close();
    pt_close();
    return ok ? 0 : 10;
}
