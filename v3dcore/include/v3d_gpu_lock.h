/*
 * ogles2: GPU submission lock, phase 1 (plan section 7.1). New in ogles2's
 * copy of the backend; not part of PiStorm3D 29.1.
 *
 * Any two GPU clients (two ogles2 programs, later ogles2 and minigl.library)
 * program the same control-list queues and wait on the same frame counters,
 * so a client holds this public semaphore from its binning submit until its
 * render has finished. Serialises the GPU; phase 2 (M6) replaces it.
 */
#ifndef V3D_GPU_LOCK_H
#define V3D_GPU_LOCK_H

#include <exec/semaphores.h>

#define V3D_GPU_LOCK_NAME "V3D 4.2 submit"

/* Finds the public semaphore, or creates it (public memory, never removed:
 * every client may still reference it). NULL if out of memory. */
struct SignalSemaphore *v3d_gpu_lock_get(void);

/* Obtains the lock, waiting at most timeout_ms. FALSE on timeout (a holder
 * that crashed or hangs): the caller reports the GPU as busy. Works from a
 * plain task (timer.device, not dos.library's Delay). */
int v3d_gpu_lock(struct SignalSemaphore *lock, unsigned long timeout_ms);
void v3d_gpu_unlock(struct SignalSemaphore *lock);

#endif
