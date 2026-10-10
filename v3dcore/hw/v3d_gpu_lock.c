/*
 * ogles2: GPU submission lock, phase 1 (plan section 7.1); see
 * include/v3d_gpu_lock.h. New in ogles2's copy of the backend.
 */
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include "../include/v3d_gpu_lock.h"

extern struct ExecBase *SysBase;

#define POLL_US 2000UL /* between attempts while another client holds it */

struct SignalSemaphore *v3d_gpu_lock_get(void)
{
    struct SignalSemaphore *s;
    static const char name[] = V3D_GPU_LOCK_NAME;

    Forbid();
    s = FindSemaphore((STRPTR)name);
    if (!s) {
        /* Public memory with the name inside it: it outlives this library. */
        s = AllocMem(sizeof(*s) + sizeof(name), MEMF_PUBLIC | MEMF_CLEAR);
        if (s) {
            char *n = (char *)(s + 1);
            CopyMem((APTR)name, n, sizeof(name));
            s->ss_Link.ln_Name = n;
            s->ss_Link.ln_Pri = 0;
            AddSemaphore(s); /* initialises it and adds it to the public list */
        }
    }
    Permit();
    return s;
}

int v3d_gpu_lock(struct SignalSemaphore *lock, unsigned long timeout_ms)
{
    struct MsgPort *port;
    struct timerequest *tr;
    unsigned long waited_us = 0, limit_us = timeout_ms * 1000UL;
    int ok = 0;

    if (!lock)
        return 0;
    if (AttemptSemaphore(lock))
        return 1;

    port = CreateMsgPort();
    tr = port ? (struct timerequest *)CreateIORequest(port, sizeof(*tr)) : 0;
    if (!tr || OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)tr, 0)) {
        /* No timer: wait without a time limit rather than not at all. */
        if (tr)
            DeleteIORequest((struct IORequest *)tr);
        if (port)
            DeleteMsgPort(port);
        ObtainSemaphore(lock);
        return 1;
    }
    while (waited_us < limit_us) {
        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs = 0;
        tr->tr_time.tv_micro = POLL_US;
        DoIO((struct IORequest *)tr);
        waited_us += POLL_US;
        if (AttemptSemaphore(lock)) {
            ok = 1;
            break;
        }
    }
    CloseDevice((struct IORequest *)tr);
    DeleteIORequest((struct IORequest *)tr);
    DeleteMsgPort(port);
    return ok;
}

void v3d_gpu_unlock(struct SignalSemaphore *lock)
{
    if (lock)
        ReleaseSemaphore(lock);
}
