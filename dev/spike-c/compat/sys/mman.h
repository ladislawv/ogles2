/* newlib's <sys/mman.h> for m68k-amigaos lacks MAP_FAILED. mmap/munmap on the
 * fake DRM device are implemented in compat/fake_drm.c. */
#ifndef OGLES2_COMPAT_SYS_MMAN_H
#define OGLES2_COMPAT_SYS_MMAN_H
#include_next <sys/mman.h>
#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif
#endif
