/*
 * Buffer objects (plan section 7.5): glGenBuffers, glBindBuffer,
 * glBufferData, glBufferSubData, and the GPU copies the draw path reads.
 *
 * A buffer keeps a CPU shadow in the application's byte order (the 68k's:
 * the data are typeless until a draw says how to read them). A draw that
 * sources an attribute from a buffer gets a GPU copy of the whole buffer
 * read with that attribute's layout (offset, size, type, stride), converted
 * once (byte-swapped, tightly packed at a 4-byte stride, as client arrays
 * are) and kept, up to OGLES2_BUFFER_COPIES layouts per buffer. A change of
 * part of the data (glBufferSubData) re-converts only the elements it
 * touches in each copy the open job does not use; a copy the open job uses
 * is retired with the job instead (renaming, plan 7.3), so nothing waits.
 * glBufferData and glUnmapBufferOES (OES_mapbuffer: the mapping is the
 * shadow itself) start every copy afresh.
 * Element buffers are read from the shadow: their indices are copied per
 * draw, as client indices are.
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_frame.h"
#include "v3d_hw.h"

#define OGLES2_BUFFER_COPIES 8

/* OES_mapbuffer (gl2ext.h) */
#ifndef GL_WRITE_ONLY_OES
#define GL_WRITE_ONLY_OES         0x88B9
#define GL_BUFFER_ACCESS_OES      0x88BB
#define GL_BUFFER_MAPPED_OES      0x88BC
#define GL_BUFFER_MAP_POINTER_OES 0x88BD
#endif

struct gpu_copy
{
    v3d_mem mem;
    ULONG serial;            /* the data serial it was made from; 0: empty */
    ULONG offset;
    GLint size;
    GLenum type;
    GLsizei stride;
    ULONG vertices;          /* how many it holds */
    ULONG dst_stride;
    ULONG used_job;          /* the job that last drew from it */
    BOOL normalized;
    ULONG elem_bytes;        /* one element as the application stores it */
    void (*convert)(UBYTE *dst, ULONG dst_stride, const UBYTE *src, ULONG src_stride, ULONG count,
                    GLint size, GLenum type); /* draw.c's, for partial updates */
};

struct OGLES2Buffer
{
    GLuint name;
    GLenum usage;
    ULONG size;
    UBYTE *data;             /* shadow, AllocVec'd; NULL while size is 0 */
    ULONG serial;            /* bumped on every change of the data */
    BOOL bound_once;         /* a buffer object exists once bound (GLES 2.0 2.9) */
    BOOL mapped;             /* OES_mapbuffer: glMapBufferOES handed out the shadow */
    struct gpu_copy copy[OGLES2_BUFFER_COPIES];
};

struct OGLES2BufState
{
    struct OGLES2Buffer **names;
    GLuint nnames;
    struct OGLES2Buffer *array, *element; /* GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER */
};

static struct OGLES2BufState *bs_get(struct OGLES2Context *ctx)
{
    if (!ctx->buf)
        ctx->buf = AllocVec(sizeof(struct OGLES2BufState), MEMF_ANY | MEMF_CLEAR);
    return ctx->buf;
}

/* Frees a GPU copy now, or after the open job if it reads it. */
static void drop_copy(struct OGLES2Context *ctx, struct gpu_copy *c)
{
    if (!c->mem.hostptr)
        return;
    if (ctx->v3d && ctx->v3d->frame_active && c->used_job == ctx->job_serial)
        v3d_frame_defer_free(&ctx->v3d->frame, &c->mem); /* empties c->mem */
    else
        v3d_mem_free(ogles2_device(), &c->mem);
    memset(c, 0, sizeof(*c));
}

static void drop_copies(struct OGLES2Context *ctx, struct OGLES2Buffer *b)
{
    int k;
    for (k = 0; k < OGLES2_BUFFER_COPIES; k++)
        drop_copy(ctx, &b->copy[k]);
}

static void free_buffer(struct OGLES2Context *ctx, struct OGLES2Buffer *b)
{
    drop_copies(ctx, b);
    if (b->data)
        FreeVec(b->data);
    FreeVec(b);
}

void ogles2_buffers_free(struct OGLES2Context *ctx)
{
    struct OGLES2BufState *bs = ctx->buf;
    GLuint i;
    if (!bs)
        return;
    for (i = 1; i < bs->nnames; i++)
        if (bs->names[i])
            free_buffer(ctx, bs->names[i]);
    if (bs->names)
        FreeVec(bs->names);
    FreeVec(bs);
    ctx->buf = 0;
}

/* ---- names and binding ---- */

static BOOL table_room(struct OGLES2BufState *bs, GLuint name)
{
    GLuint nn, j;
    struct OGLES2Buffer **tab;
    if (name < bs->nnames)
        return TRUE;
    nn = bs->nnames ? bs->nnames : 32;
    while (nn <= name)
        nn *= 2;
    if (!(tab = AllocVec(nn * sizeof(*tab), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    for (j = 0; j < bs->nnames; j++)
        tab[j] = bs->names[j];
    if (bs->names)
        FreeVec(bs->names);
    bs->names = tab;
    bs->nnames = nn;
    return TRUE;
}

static struct OGLES2Buffer *create(struct OGLES2BufState *bs, GLuint name)
{
    struct OGLES2Buffer *b;
    if (!table_room(bs, name) || !(b = AllocVec(sizeof(*b), MEMF_ANY | MEMF_CLEAR)))
        return 0;
    b->name = name;
    b->usage = GL_STATIC_DRAW;
    bs->names[name] = b;
    return b;
}

static struct OGLES2Buffer *by_name(struct OGLES2BufState *bs, GLuint name)
{
    return name && name < bs->nnames ? bs->names[name] : 0;
}

void ogles2_glGenBuffers(struct OGLES2Context *ctx, GLsizei n, GLuint *buffers)
{
    struct OGLES2BufState *bs;
    GLsizei k;
    GLuint i = 1;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(bs = bs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < n; k++) {
        while (i < bs->nnames && bs->names[i])
            i++;
        if (!create(bs, i)) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        if (buffers)
            buffers[k] = i;
    }
}

void ogles2_glDeleteBuffers(struct OGLES2Context *ctx, GLsizei n, const GLuint *buffers)
{
    struct OGLES2BufState *bs;
    GLsizei k;
    int a;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(bs = bs_get(ctx)) || !buffers)
        return;
    for (k = 0; k < n; k++) {
        struct OGLES2Buffer *b = by_name(bs, buffers[k]);
        if (!b)
            continue;
        /* GLES 2.0 2.9: a deleted buffer is unbound wherever it is bound
         * in the context, attribute arrays included. */
        if (bs->array == b)
            bs->array = 0;
        if (bs->element == b)
            bs->element = 0;
        for (a = 0; a < OGLES2_MAX_VERTEX_ATTRIBS; a++)
            if (ctx->array[a].buffer == b) {
                ctx->array[a].buffer = 0;
                ctx->array[a].pointer = 0;
            }
        bs->names[b->name] = 0;
        free_buffer(ctx, b);
    }
}

GLboolean ogles2_glIsBuffer(struct OGLES2Context *ctx, GLuint buffer)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return GL_FALSE;
    b = by_name(bs, buffer);
    return b && b->bound_once ? GL_TRUE : GL_FALSE;
}

void ogles2_glBindBuffer(struct OGLES2Context *ctx, GLenum target, GLuint buffer)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b = 0;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return;
    if (target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (buffer) {
        b = by_name(bs, buffer);
        /* GLES2 lets a program bind a name glGenBuffers did not return. */
        if (!b && !(b = create(bs, buffer))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        b->bound_once = TRUE;
    }
    if (target == GL_ARRAY_BUFFER)
        bs->array = b;
    else
        bs->element = b;
}

static struct OGLES2Buffer *bound(struct OGLES2Context *ctx, struct OGLES2BufState *bs, GLenum target)
{
    if (target == GL_ARRAY_BUFFER)
        return bs->array;
    if (target == GL_ELEMENT_ARRAY_BUFFER)
        return bs->element;
    ogles2_error(ctx, GL_INVALID_ENUM);
    return 0;
}

void ogles2_glBufferData(struct OGLES2Context *ctx, GLenum target, GLsizeiptr size, const void *data,
                         GLenum usage)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    UBYTE *shadow = 0;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return;
    if (target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (usage != GL_STREAM_DRAW && usage != GL_STATIC_DRAW && usage != GL_DYNAMIC_DRAW) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (size < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(b = bound(ctx, bs, target))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (size && !(shadow = AllocVec((ULONG)size, MEMF_ANY | MEMF_CLEAR))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (shadow && data)
        memcpy(shadow, data, (ULONG)size);
    drop_copies(ctx, b);
    if (b->data)
        FreeVec(b->data);
    b->data = shadow;
    b->size = (ULONG)size;
    b->usage = usage;
    b->mapped = FALSE; /* new storage: an old mapping is gone */
    b->serial++;
}

/* Bytes [off, off+len) of the shadow changed (plan 7.5): each copy the open
 * job does not read re-converts the elements those bytes touch, in place;
 * a copy the open job reads is retired with it and rebuilt at its next use.
 * The data serial has been bumped already. */
static void patch_copies(struct OGLES2Context *ctx, struct OGLES2Buffer *b, ULONG off, ULONG len)
{
    int k;
    for (k = 0; k < OGLES2_BUFFER_COPIES; k++) {
        struct gpu_copy *c = &b->copy[k];
        ULONG stride, i0, i1, n;
        if (!c->mem.hostptr)
            continue;
        if (c->serial != b->serial - 1 || !c->convert ||
            (ctx->v3d && ctx->v3d->frame_active && c->used_job == ctx->job_serial)) {
            drop_copy(ctx, c);
            continue;
        }
        c->serial = b->serial;
        stride = c->stride ? (ULONG)c->stride : c->elem_bytes;
        /* Element i spans [c->offset + i*stride, + elem_bytes). */
        if (off + len <= c->offset)
            continue;
        i0 = off + 1 > c->offset + c->elem_bytes ? (off + 1 - c->offset - c->elem_bytes + stride - 1) / stride : 0;
        i1 = (off + len - 1 - c->offset) / stride;
        if (i1 >= c->vertices)
            i1 = c->vertices - 1;
        if (i0 > i1)
            continue;
        n = i1 - i0 + 1;
        c->convert((UBYTE *)c->mem.hostptr + i0 * c->dst_stride, c->dst_stride,
                   b->data + c->offset + i0 * stride, stride, n, c->size, c->type);
        {
            ULONG flush = n * c->dst_stride;
            CachePreDMA((UBYTE *)c->mem.hostptr + i0 * c->dst_stride, &flush, 0);
        }
    }
}

void ogles2_glBufferSubData(struct OGLES2Context *ctx, GLenum target, GLintptr offset, GLsizeiptr size,
                            const void *data)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return;
    if (target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(b = bound(ctx, bs, target))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (offset < 0 || size < 0 || (ULONG)offset + (ULONG)size > b->size) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!size || !data)
        return;
    memcpy(b->data + offset, data, (ULONG)size);
    b->serial++;
    patch_copies(ctx, b, (ULONG)offset, (ULONG)size);
}

void ogles2_glGetBufferParameteriv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                   GLint *params)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_context_valid(ctx) || !params || !(bs = bs_get(ctx)))
        return;
    if (target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(b = bound(ctx, bs, target))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (pname == GL_BUFFER_SIZE)
        *params = (GLint)b->size;
    else if (pname == GL_BUFFER_USAGE)
        *params = (GLint)b->usage;
    else if (pname == GL_BUFFER_ACCESS_OES)
        *params = GL_WRITE_ONLY_OES;
    else if (pname == GL_BUFFER_MAPPED_OES)
        *params = b->mapped ? GL_TRUE : GL_FALSE;
    else
        ogles2_error(ctx, GL_INVALID_ENUM);
}

/* ---- OES_mapbuffer: the mapping is the shadow ---- */

void *ogles2_glMapBufferOES(struct OGLES2Context *ctx, GLenum target, GLenum access)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return 0;
    if ((target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) || access != GL_WRITE_ONLY_OES) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    if (!(b = bound(ctx, bs, target)) || b->mapped) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return 0;
    }
    b->mapped = TRUE;
    return b->data;
}

GLboolean ogles2_glUnmapBufferOES(struct OGLES2Context *ctx, GLenum target)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return GL_FALSE;
    if (target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return GL_FALSE;
    }
    if (!(b = bound(ctx, bs, target)) || !b->mapped) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    b->mapped = FALSE;
    /* Anything may have changed: every copy starts afresh. */
    drop_copies(ctx, b);
    b->serial++;
    return GL_TRUE;
}

void ogles2_glGetBufferPointervOES(struct OGLES2Context *ctx, GLenum target, GLenum pname, void **params)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_context_valid(ctx) || !params || !(bs = bs_get(ctx)))
        return;
    if ((target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) ||
        pname != GL_BUFFER_MAP_POINTER_OES) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(b = bound(ctx, bs, target))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    *params = b->mapped ? b->data : 0;
}

/* ---- for the rest of the library ---- */

BOOL ogles2_buffer_query(struct OGLES2Context *ctx, GLenum pname, GLint *v)
{
    struct OGLES2BufState *bs = bs_get(ctx);
    if (pname == GL_ARRAY_BUFFER_BINDING) {
        *v = bs && bs->array ? (GLint)bs->array->name : 0;
        return TRUE;
    }
    if (pname == GL_ELEMENT_ARRAY_BUFFER_BINDING) {
        *v = bs && bs->element ? (GLint)bs->element->name : 0;
        return TRUE;
    }
    return FALSE;
}

/* The GL_ARRAY_BUFFER binding (glVertexAttribPointer captures it). */
struct OGLES2Buffer *ogles2_buffer_array(struct OGLES2Context *ctx)
{
    struct OGLES2BufState *bs = bs_get(ctx);
    return bs ? bs->array : 0;
}

GLuint ogles2_buffer_name(const struct OGLES2Buffer *b)
{
    return b ? b->name : 0;
}

ULONG ogles2_buffer_size(const struct OGLES2Buffer *b)
{
    return b ? b->size : 0;
}

/* glDrawElements' indices in the GL_ELEMENT_ARRAY_BUFFER: `offset` bytes
 * into it, `bytes` long; NULL without an element buffer (the indices are
 * then the application's pointer) or with the range outside it (*bad). */
const void *ogles2_buffer_indices(struct OGLES2Context *ctx, ULONG offset, ULONG bytes, BOOL *bad)
{
    struct OGLES2BufState *bs = bs_get(ctx);
    *bad = FALSE;
    if (!bs || !bs->element)
        return 0;
    if (!bs->element->data || offset + bytes > bs->element->size || offset + bytes < offset) {
        *bad = TRUE;
        return 0;
    }
    return bs->element->data + offset;
}

/* A GPU copy of buffer `b` read as an attribute array of `size` components
 * of `type` at `offset` with `stride` (0: packed): converted with
 * `convert` (draw.c's, as for client arrays) into tightly packed elements.
 * Returns its GPU address of vertex 0 and its stride; 0 if out of memory or
 * the buffer holds no complete element at `offset`. *vertices: how many. */
ULONG ogles2_buffer_copy(struct OGLES2Context *ctx, struct OGLES2Buffer *b, ULONG offset, GLint size,
                         GLenum type, GLboolean normalized, GLsizei stride, ULONG elem_bytes,
                         ULONG *dst_stride, ULONG *vertices,
                         void (*convert)(UBYTE *dst, ULONG dst_stride, const UBYTE *src,
                                         ULONG src_stride, ULONG count, GLint size, GLenum type))
{
    struct gpu_copy *c, *victim = 0;
    ULONG src_stride = stride ? (ULONG)stride : elem_bytes, n, len;
    int k;
    for (k = 0; k < OGLES2_BUFFER_COPIES; k++) {
        c = &b->copy[k];
        if (c->serial == b->serial && c->mem.hostptr && c->offset == offset && c->size == size &&
            c->type == type && c->stride == stride && c->normalized == normalized) {
            c->used_job = ctx->job_serial;
            *dst_stride = c->dst_stride;
            *vertices = c->vertices;
            return (ULONG)c->mem.hostptr;
        }
        if (!c->mem.hostptr && !victim)
            victim = c;
    }
    if (!victim) { /* all in use for other layouts: replace the first */
        victim = &b->copy[0];
        drop_copy(ctx, victim);
    }
    if (!b->data || offset + elem_bytes > b->size)
        return 0;
    n = (b->size - offset - elem_bytes) / src_stride + 1;
    c = victim;
    c->dst_stride = (elem_bytes + 3) & ~3UL;
    if (v3d_mem_alloc(ogles2_device(), &c->mem, c->dst_stride * n) < 0) {
        memset(c, 0, sizeof(*c));
        return 0;
    }
    convert((UBYTE *)c->mem.hostptr, c->dst_stride, b->data + offset, src_stride, n, size, type);
    len = c->mem.size;
    CachePreDMA(c->mem.hostptr, &len, 0);
    c->serial = b->serial;
    c->elem_bytes = elem_bytes;
    c->convert = convert;
    c->offset = offset;
    c->size = size;
    c->type = type;
    c->stride = stride;
    c->normalized = normalized;
    c->vertices = n;
    c->used_job = ctx->job_serial;
    *dst_stride = c->dst_stride;
    *vertices = n;
    return (ULONG)c->mem.hostptr;
}
