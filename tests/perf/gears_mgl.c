/*
 * The gears speed comparison, minigl.library side (PiStorm3D 29.1's MiniGL,
 * the baseline of plan section 1; perf_gears.h). Built against the
 * PiStorm3D 29.1 SDK (make perf, MGL_SDK).
 *   usage: gears_mgl [BITMAP|WINDOW|FULLSCREEN] [SECONDS=5] [TEETH=1] [PATH=IMMEDIATE|ARRAYS|LIST]
 * IMMEDIATE: glBegin/glNormal3fv/glVertex3fv/glEnd every frame; ARRAYS:
 * glVertexPointer/glNormalPointer and glDrawArrays; LIST: display lists
 * recorded once (glxgears' way). The same triangles as gears_gles2, lit by
 * GL's fixed function (GL_LIGHT0 from (5, 5, 10), GL_AMBIENT_AND_DIFFUSE).
 * Every glBegin block and glDrawArrays call stays below MiniGL's vertex
 * buffer (CHUNK of VBUF): MiniGL truncates bigger ones.
 *
 * Built with GEARS_GL4ES (gears_gl4es, M6): the same desktop GL program on
 * gl4es over ogles2.library (ports/gl4es), its context through agl*.
 */
#ifdef GEARS_GL4ES
#include <GL/gl.h>
#include <gl4es_agl.h>
#include <proto/cybergraphics.h>
#define LIB "gl4es"
#else
#include <proto/minigl.h>
#define LIB "MiniGL"
#endif
#include "perf_target.h"

#define VBUF 1002
#define CHUNK 999 /* vertices, whole triangles */

enum { IMMEDIATE, ARRAYS, LIST };

static void draw_immediate(const struct pg_gear *g)
{
    int i, j, n;
    for (i = 0; i < g->count; i += CHUNK) {
        n = g->count - i < CHUNK ? g->count - i : CHUNK;
        glBegin(GL_TRIANGLES);
        for (j = 0; j < n; j++) {
            float *v = g->v + (i + j) * 6;
            glNormal3fv(v + 3);
            glVertex3fv(v);
        }
        glEnd();
    }
}

static void draw_arrays(const struct pg_gear *g)
{
    int i;
    glVertexPointer(3, GL_FLOAT, 24, g->v);
    glNormalPointer(GL_FLOAT, 24, g->v + 3);
    for (i = 0; i < g->count; i += CHUNK)
        glDrawArrays(GL_TRIANGLES, i, g->count - i < CHUNK ? g->count - i : CHUNK);
}

static int path;
static GLuint lists;

static void frame(float angle)
{
    int k;
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -40.0f);
    glRotatef(PG_VIEW_ROTX, 1.0f, 0.0f, 0.0f);
    glRotatef(PG_VIEW_ROTY, 0.0f, 1.0f, 0.0f);
    for (k = 0; k < 3; k++) {
        float tx, ty, rz;
        pg_place(k, angle, &tx, &ty, &rz);
        glPushMatrix();
        glTranslatef(tx, ty, 0.0f);
        glRotatef(rz, 0.0f, 0.0f, 1.0f);
        glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, pg_gears[k].color);
        if (path == LIST)
            glCallList(lists + k);
        else if (path == ARRAYS)
            draw_arrays(&pg_gears[k]);
        else
            draw_immediate(&pg_gears[k]);
        glPopMatrix();
    }
}

int main(int argc, char **argv)
{
    static GLfloat light[4] = {5.0f, 5.0f, 10.0f, 0.0f};
    struct pg_args a;
    ULONG frames = 0, t0 = 0, t1;
    float angle = 0.0f;
    int k, ok = 0;
    void *ctx = 0;
    char title[80];

    pg_parse(argc, argv, &a, "IMMEDIATE");
    path = pg_eq(a.path, "ARRAYS") ? ARRAYS : pg_eq(a.path, "LIST") ? LIST : IMMEDIATE;
    a.path = path == ARRAYS ? "ARRAYS" : path == LIST ? "LIST" : "IMMEDIATE";
    if (!pg_scene(a.mult)) {
        printf("out of memory\n");
        return 20;
    }
    sprintf(title, LIB " %s: %d triangles", a.path, pg_triangles());
    if (!pt_open(&a, title)) {
        printf("cannot open the window or bitmap\n");
        pt_close();
        return 20;
    }
#ifdef GEARS_GL4ES
    {
        ULONG err = 0;
        if (a.fullscreen)
            ctx = aglCreateContextTags2(&err, OGLES2_CCT_MODEID, pt_find_mode(), OGLES2_CCT_DEPTH, 24,
                                        OGLES2_CCT_VSYNC, FALSE, TAG_DONE);
        else
            ctx = aglCreateContextTags2(&err, a.window ? OGLES2_CCT_WINDOW : OGLES2_CCT_BITMAP,
                                        a.window ? (ULONG)pt_win : (ULONG)pt_bm, OGLES2_CCT_DEPTH, 24,
                                        OGLES2_CCT_VSYNC, FALSE, TAG_DONE);
        if (!ctx) {
            printf("gl4es: no context (%lu); needs ogles2.library\n", (unsigned long)err);
            pt_close();
            return 20;
        }
        aglMakeCurrent(ctx);
    }
#else
    if (!MiniGLOpen()) {
        printf("needs minigl.library 29.1 (PiStorm3D)\n");
        pt_close();
        return 20;
    }
    mglChooseVertexBufferSize(VBUF);
    if (a.fullscreen) {
        mglChoosePixelDepth(32);
        ctx = mglCreateContext(0, 0, PG_W, PG_H); /* a screen of its own */
    } else {
        ctx = a.window ? mglCreateContextFromWindow(pt_win) : mglCreateContextFromBitMap(pt_bm);
    }
    if (!ctx) {
        printf("MiniGL: no context\n");
        goto out;
    }
    mglEnableSync(GL_FALSE);
#endif

    glViewport(0, 0, PG_W, PG_H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -(double)PG_H / PG_W, (double)PG_H / PG_W, 5.0, 60.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, light); /* eye space: the modelview is the identity */
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);
    glShadeModel(GL_SMOOTH);
    glClearColor(0, 0, 0, 1);
    if (path == ARRAYS) {
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
    }
    if (path == LIST) {
        lists = glGenLists(3);
        for (k = 0; k < 3; k++) {
            glNewList(lists + k, GL_COMPILE);
            draw_immediate(&pg_gears[k]);
            glEndList();
        }
    }

    while (!pt_break()) {
        if (frames == 5) /* as gears_gles2: the first frames are not timed */
            t0 = pt_ticks();
        frame(angle);
#ifdef GEARS_GL4ES
        aglSwapBuffers();
#else
        mglSwitchDisplay();
#endif
        angle += PG_ANGLE_STEP;
        frames++;
        if (frames > 5 && pt_ticks() - t0 >= (ULONG)a.seconds * 50)
            break;
    }
    glFinish();
    t1 = pt_ticks();
    if (frames > 5) {
        pt_report(LIB, &a, frames - 5, t1 - t0);
        frame(0.0f);
#ifdef GEARS_GL4ES
        aglSwapBuffers();
#else
        mglSwitchDisplay();
#endif
        glFinish();
        pt_image(LIB);
        ok = 1;
    }
#ifdef GEARS_GL4ES
    aglDestroyContext(ctx);
    pt_close(); /* gl4es and ogles2 close at exit (agl_os3.c) */
#else
out:
    if (ctx)
        mglDeleteContext();
    /* gl.h: the window closes before MiniGLClose (MGLTerm). */
    pt_close();
    MiniGLClose();
#endif
    return ok ? 0 : 10;
}
