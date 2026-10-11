/*
 * Stand-in for libdrm's <xf86drm.h>: the subset the V3D driver calls.
 * Implemented by compat/fake_drm.c, a device that answers like a V3D 4.2 but
 * runs nothing (shader compilation only). Mesa's drm-shim
 * (src/broadcom/drm-shim/v3d_noop.c) is the model.
 */
#ifndef OGLES2_COMPAT_XF86DRM_H
#define OGLES2_COMPAT_XF86DRM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int drmIoctl(int fd, unsigned long request, void *arg);

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
                   int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);

/* Opens the fake device; returns its file descriptor number. */
int fake_drm_open(void);

#ifdef __cplusplus
}
#endif

#endif
