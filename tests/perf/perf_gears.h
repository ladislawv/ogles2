/*
 * The M4 speed comparison (plan section 9, M4 exit: "the VBO path beats
 * MiniGL on an equivalent gears scene"): the scene of Brian Paul's gears
 * (glxgears, es2gears), shared by gears_gles2.c (ogles2.library) and
 * gears_mgl.c (PiStorm3D's minigl.library), so both draw the same triangles
 * with the same lighting into the same kind of target.
 *
 * Geometry: each gear as independent triangles (x, y, z, nx, ny, nz), its
 * flat faces with the face's normal at every corner, as glxgears' GL_FLAT
 * quads look, the inner cylinder smooth. TEETH multiplies the tooth counts
 * (20, 10, 10) for a heavier scene. Lighting: one directional light from
 * (5, 5, 10) in eye space, diffuse, plus GL's default global ambient 0.2.
 *
 * Header only, no GL: the two programs include it with their own GL headers.
 */
#ifndef PERF_GEARS_H
#define PERF_GEARS_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PG_W 640
#define PG_H 480

struct pg_gear {
    float *v;     /* count vertices of 6 floats */
    int count;
    float color[4];
};

struct pg_builder {
    struct pg_gear *g;
    int cap;
    float n[3];       /* current normal */
    float q[4][6];    /* the quad being assembled */
    int nq;
};

static void pg_put(struct pg_builder *b, const float *p)
{
    if (b->g->count == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 1024;
        b->g->v = (float *)realloc(b->g->v, (size_t)b->cap * 6 * sizeof(float));
    }
    memcpy(b->g->v + b->g->count * 6, p, 6 * sizeof(float));
    b->g->count++;
}

static void pg_normal(struct pg_builder *b, float x, float y, float z)
{
    b->n[0] = x;
    b->n[1] = y;
    b->n[2] = z;
}

static void pg_vert(struct pg_builder *b, float x, float y, float z)
{
    float *q = b->q[b->nq++];
    q[0] = x; q[1] = y; q[2] = z;
    q[3] = b->n[0]; q[4] = b->n[1]; q[5] = b->n[2];
}

static int pg_same(const float *p, const float *q)
{
    return p[0] == q[0] && p[1] == q[1] && p[2] == q[2];
}

/* A triangle, unless two corners coincide (glxgears' front and back faces
 * have such quads: drawn, they cover nothing). */
static void pg_tri(struct pg_builder *b, const float *p, const float *q, const float *r)
{
    if (pg_same(p, q) || pg_same(q, r) || pg_same(p, r))
        return;
    pg_put(b, p);
    pg_put(b, q);
    pg_put(b, r);
}

/* A quad from GL_QUADS order (a b c d), counter-clockwise from the front;
 * flat: every corner gets d's normal (GL's provoking vertex for quads). */
static void pg_quad(struct pg_builder *b, float (*a)[6], float (*bb)[6], float (*c)[6], float (*d)[6], int flat)
{
    float t[4][6];
    int k;
    memcpy(t[0], *a, sizeof(t[0]));
    memcpy(t[1], *bb, sizeof(t[1]));
    memcpy(t[2], *c, sizeof(t[2]));
    memcpy(t[3], *d, sizeof(t[3]));
    if (flat)
        for (k = 0; k < 3; k++)
            memcpy(t[k] + 3, t[3] + 3, 3 * sizeof(float));
    pg_tri(b, t[0], t[1], t[2]);
    pg_tri(b, t[0], t[2], t[3]);
}

/* GL_QUADS: every 4 vertices a quad. */
static void pg_quads_vert(struct pg_builder *b, float x, float y, float z)
{
    pg_vert(b, x, y, z);
    if (b->nq == 4) {
        pg_quad(b, &b->q[0], &b->q[1], &b->q[2], &b->q[3], 1);
        b->nq = 0;
    }
}

/* GL_QUAD_STRIP: v0 v1 v2 v3 is the quad v0 v1 v3 v2. */
static void pg_strip_vert(struct pg_builder *b, float x, float y, float z, int flat)
{
    pg_vert(b, x, y, z);
    if (b->nq == 4) {
        pg_quad(b, &b->q[0], &b->q[1], &b->q[3], &b->q[2], flat);
        memcpy(b->q[0], b->q[2], sizeof(b->q[0]));
        memcpy(b->q[1], b->q[3], sizeof(b->q[1]));
        b->nq = 2;
    }
}

/* glxgears' gear(), recorded as triangles. */
static void pg_build(struct pg_gear *g, float inner, float outer, float width, int teeth, float depth)
{
    struct pg_builder b;
    float r0 = inner, r1 = outer - depth / 2.0f, r2 = outer + depth / 2.0f;
    float da = 2.0f * (float)M_PI / teeth / 4.0f, w = width * 0.5f, a, u, v, len;
    int i;

    memset(&b, 0, sizeof(b));
    g->v = 0;
    g->count = 0;
    b.g = g;

    /* front face */
    pg_normal(&b, 0, 0, 1);
    for (i = 0; i <= teeth; i++) {
        a = i * 2.0f * (float)M_PI / teeth;
        pg_strip_vert(&b, r0 * cosf(a), r0 * sinf(a), w, 1);
        pg_strip_vert(&b, r1 * cosf(a), r1 * sinf(a), w, 1);
        if (i < teeth) {
            pg_strip_vert(&b, r0 * cosf(a), r0 * sinf(a), w, 1);
            pg_strip_vert(&b, r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), w, 1);
        }
    }
    b.nq = 0;
    /* front sides of teeth */
    for (i = 0; i < teeth; i++) {
        a = i * 2.0f * (float)M_PI / teeth;
        pg_quads_vert(&b, r1 * cosf(a), r1 * sinf(a), w);
        pg_quads_vert(&b, r2 * cosf(a + da), r2 * sinf(a + da), w);
        pg_quads_vert(&b, r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), w);
        pg_quads_vert(&b, r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), w);
    }

    /* back face */
    pg_normal(&b, 0, 0, -1);
    for (i = 0; i <= teeth; i++) {
        a = i * 2.0f * (float)M_PI / teeth;
        pg_strip_vert(&b, r1 * cosf(a), r1 * sinf(a), -w, 1);
        pg_strip_vert(&b, r0 * cosf(a), r0 * sinf(a), -w, 1);
        if (i < teeth) {
            pg_strip_vert(&b, r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), -w, 1);
            pg_strip_vert(&b, r0 * cosf(a), r0 * sinf(a), -w, 1);
        }
    }
    b.nq = 0;
    /* back sides of teeth */
    for (i = 0; i < teeth; i++) {
        a = i * 2.0f * (float)M_PI / teeth;
        pg_quads_vert(&b, r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), -w);
        pg_quads_vert(&b, r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), -w);
        pg_quads_vert(&b, r2 * cosf(a + da), r2 * sinf(a + da), -w);
        pg_quads_vert(&b, r1 * cosf(a), r1 * sinf(a), -w);
    }

    /* outward faces of teeth */
    for (i = 0; i < teeth; i++) {
        a = i * 2.0f * (float)M_PI / teeth;
        pg_strip_vert(&b, r1 * cosf(a), r1 * sinf(a), w, 1);
        pg_strip_vert(&b, r1 * cosf(a), r1 * sinf(a), -w, 1);
        u = r2 * cosf(a + da) - r1 * cosf(a);
        v = r2 * sinf(a + da) - r1 * sinf(a);
        len = sqrtf(u * u + v * v);
        u /= len;
        v /= len;
        pg_normal(&b, v, -u, 0);
        pg_strip_vert(&b, r2 * cosf(a + da), r2 * sinf(a + da), w, 1);
        pg_strip_vert(&b, r2 * cosf(a + da), r2 * sinf(a + da), -w, 1);
        pg_normal(&b, cosf(a), sinf(a), 0);
        pg_strip_vert(&b, r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), w, 1);
        pg_strip_vert(&b, r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), -w, 1);
        u = r1 * cosf(a + 3 * da) - r2 * cosf(a + 2 * da);
        v = r1 * sinf(a + 3 * da) - r2 * sinf(a + 2 * da);
        pg_normal(&b, v, -u, 0);
        pg_strip_vert(&b, r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), w, 1);
        pg_strip_vert(&b, r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), -w, 1);
        pg_normal(&b, cosf(a), sinf(a), 0);
    }
    pg_strip_vert(&b, r1 * cosf(0), r1 * sinf(0), w, 1);
    pg_strip_vert(&b, r1 * cosf(0), r1 * sinf(0), -w, 1);
    b.nq = 0;

    /* inside radius cylinder, smooth */
    for (i = 0; i <= teeth; i++) {
        a = i * 2.0f * (float)M_PI / teeth;
        pg_normal(&b, -cosf(a), -sinf(a), 0);
        pg_strip_vert(&b, r0 * cosf(a), r0 * sinf(a), -w, 0);
        pg_strip_vert(&b, r0 * cosf(a), r0 * sinf(a), w, 0);
    }
}

/* The scene: three gears, their places and colours (glxgears). */
static struct pg_gear pg_gears[3];

static int pg_scene(int mult)
{
    static const float colors[3][4] = {{0.8f, 0.1f, 0.0f, 1.0f}, {0.0f, 0.8f, 0.2f, 1.0f}, {0.2f, 0.2f, 1.0f, 1.0f}};
    int k;
    pg_build(&pg_gears[0], 1.0f, 4.0f, 1.0f, 20 * mult, 0.7f);
    pg_build(&pg_gears[1], 0.5f, 2.0f, 2.0f, 10 * mult, 0.7f);
    pg_build(&pg_gears[2], 1.3f, 2.0f, 0.5f, 10 * mult, 0.7f);
    for (k = 0; k < 3; k++) {
        if (!pg_gears[k].v)
            return 0;
        memcpy(pg_gears[k].color, colors[k], sizeof(colors[k]));
    }
    return 1;
}

static int pg_triangles(void)
{
    return (pg_gears[0].count + pg_gears[1].count + pg_gears[2].count) / 3;
}

/* Gear k's place at rotation angle (degrees): translate, then rotate about z. */
static void pg_place(int k, float angle, float *tx, float *ty, float *rz)
{
    static const float pos[3][2] = {{-3.0f, -2.0f}, {3.1f, -2.0f}, {-3.1f, 4.2f}};
    *tx = pos[k][0];
    *ty = pos[k][1];
    *rz = k == 0 ? angle : k == 1 ? -2.0f * angle - 9.0f : -2.0f * angle - 25.0f;
}

#define PG_VIEW_ROTX 20.0f
#define PG_VIEW_ROTY 30.0f
#define PG_ANGLE_STEP 2.0f /* degrees per frame */

/* ---- arguments: [BITMAP|WINDOW] [SECONDS=n] [TEETH=n] [PATH=name] ---- */
struct pg_args {
    int window;
    int seconds;
    int mult;
    const char *path;
};

static int pg_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20))
            return 0;
    return *a == *b;
}

static void pg_parse(int argc, char **argv, struct pg_args *a, const char *default_path)
{
    int i;
    a->window = 0;
    a->seconds = 5;
    a->mult = 1;
    a->path = default_path;
    for (i = 1; i < argc; i++) {
        if (pg_eq(argv[i], "WINDOW"))
            a->window = 1;
        else if (pg_eq(argv[i], "BITMAP"))
            a->window = 0;
        else if (!strncmp(argv[i], "SECONDS=", 8))
            a->seconds = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "TEETH=", 6))
            a->mult = atoi(argv[i] + 6);
        else if (!strncmp(argv[i], "PATH=", 5))
            a->path = argv[i] + 5;
    }
    if (a->seconds < 1)
        a->seconds = 1;
    if (a->mult < 1)
        a->mult = 1;
}

#endif
