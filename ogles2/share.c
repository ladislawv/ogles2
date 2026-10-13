/*
 * Share groups (OGLES2_CCT_SHARE_WITH; GLES 2.0 appendix C; plan 13, M6).
 *
 * Every context belongs to a group, alone in it unless it was created to
 * share with another. The group holds the name tables of the objects GLES2
 * shares: textures, buffers, shaders and programs (with the program cache's
 * images) and renderbuffers (struct OGLES2Share). Framebuffers, the default
 * textures and every binding stay the context's own.
 *
 * What a job not rendered yet may read is looked for in every member's jobs:
 * job serials are unique across contexts (job.c), so an object's "last used
 * by job N" finds the member whose open or running job N is, and a block
 * that job reads is freed after its render (that member's retire list).
 *
 * The members may be used from several tasks (a loader task with a context
 * of its own, say). A change to a shared object can render, wait for or
 * defer through another member's job, so while a group has two members or
 * more every call into a member runs under the group's semaphore: the
 * dispatch table's wrappers take it (gen/gen_ogles2.py, through
 * ctx->share_lock), aglDestroyContext and aglCreateContext2 (joining) take
 * it themselves. A context alone in its group pays one test a call. Create
 * the contexts of a group before using them from several tasks: a call
 * already running in another task when the second member joins is not
 * waited for.
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "v3d_context.h"

/* Every member takes the group's semaphore while there are two or more. */
static void set_locks(struct OGLES2Share *sh)
{
    struct OGLES2Context *m;
    for (m = sh->first; m; m = m->share_next)
        m->share_lock = sh->members > 1 ? &sh->lock : 0;
}

BOOL ogles2_share_join(struct OGLES2Context *ctx, struct OGLES2Context *with)
{
    struct OGLES2Share *sh;
    if (with) {
        sh = with->share;
        ObtainSemaphore(&sh->lock); /* the members' calls walk the member list */
        ctx->share = sh;
        ctx->share_next = sh->first;
        sh->first = ctx;
        sh->members++;
        set_locks(sh);
        ReleaseSemaphore(&sh->lock);
        return TRUE;
    }
    if (!(sh = AllocVec(sizeof(*sh), MEMF_PUBLIC | MEMF_CLEAR)))
        return FALSE;
    InitSemaphore(&sh->lock);
    ctx->share = sh;
    ctx->share_next = 0;
    sh->first = ctx;
    sh->members = 1;
    ctx->share_lock = 0;
    return TRUE;
}

/* The caller holds the group's semaphore when it has other members
 * (aglDestroyContext). */
BOOL ogles2_share_leave(struct OGLES2Context *ctx)
{
    struct OGLES2Share *sh = ctx->share;
    struct OGLES2Context **pp;
    if (!sh)
        return TRUE;
    for (pp = &sh->first; *pp; pp = &(*pp)->share_next)
        if (*pp == ctx) {
            *pp = ctx->share_next;
            break;
        }
    ctx->share = 0;
    ctx->share_next = 0;
    ctx->share_lock = 0;
    if (--sh->members) {
        set_locks(sh);
        return FALSE;
    }
    /* The modules freed the objects and their tables (members was 1). */
    FreeVec(sh);
    return TRUE;
}

struct OGLES2Context *ogles2_job_user(struct OGLES2Context *ctx, ULONG serial)
{
    struct OGLES2Context *m;
    if (!serial)
        return 0;
    m = ctx->share ? ctx->share->first : ctx;
    for (; m; m = ctx->share ? m->share_next : 0)
        if ((m->v3d && m->v3d->frame_active && serial == m->job_serial) || (m->pend && serial == m->pend_serial))
            return m;
    return 0;
}

void ogles2_share_defer_free(struct OGLES2Context *ctx, ULONG serial, struct v3d_mem *m)
{
    struct OGLES2Context *user;
    if (!m->hostptr)
        return;
    if ((user = ogles2_job_user(ctx, serial)) && user->v3d)
        v3d_frame_defer_free(&user->v3d->frame, m);
    else
        v3d_mem_free(ogles2_device(), m);
    memset(m, 0, sizeof(*m));
}

/* ---- name tables ----
 * A table of objects by name is an array: the object named n is at n. A
 * program may bind a name it never generated (GLES 2.0 4.4.1), and dEQP
 * binds random 32-bit ones: a table that grew to such a name took hundreds
 * of megabytes, or failed. A name from OGLES2_NEAR_NAMES on gets a slot of
 * its own past them instead, found through the table's short list. The
 * objects keep their names; loops over a table see every slot. */
GLuint ogles2_name_slot(struct OGLES2FarName **far, GLuint name, BOOL create)
{
    struct OGLES2FarName *f;
    GLuint slot = OGLES2_NEAR_NAMES;
    BOOL taken;
    if (name < OGLES2_NEAR_NAMES)
        return name;
    for (f = *far; f; f = f->next)
        if (f->name == name)
            return f->slot;
    if (!create)
        return OGLES2_NO_SLOT;
    do { /* the lowest far slot no name holds */
        taken = FALSE;
        for (f = *far; f; f = f->next)
            if (f->slot == slot) {
                taken = TRUE;
                slot++;
            }
    } while (taken);
    if (!(f = AllocVec(sizeof(*f), MEMF_ANY)))
        return OGLES2_NO_SLOT;
    f->name = name;
    f->slot = slot;
    f->next = *far;
    *far = f;
    return slot;
}

void ogles2_name_forget(struct OGLES2FarName **far, GLuint name)
{
    struct OGLES2FarName **p, *f;
    for (p = far; (f = *p); p = &f->next)
        if (f->name == name) {
            *p = f->next;
            FreeVec(f);
            return;
        }
}

void ogles2_names_free(struct OGLES2FarName **far)
{
    struct OGLES2FarName *f;
    while ((f = *far)) {
        *far = f->next;
        FreeVec(f);
    }
}
