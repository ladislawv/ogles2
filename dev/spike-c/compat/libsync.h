/*
 * Stand-in for Android's libsync.h (Linux sync files). There is no kernel
 * fence on AmigaOS: the fake DRM device completes every job at once, so
 * every sync file is already signalled.
 */
#ifndef OGLES2_COMPAT_LIBSYNC_H
#define OGLES2_COMPAT_LIBSYNC_H

static inline int sync_wait(int fd, int timeout)
{
    (void)fd;
    (void)timeout;
    return 0;
}

static inline int sync_merge(const char *name, int fd1, int fd2)
{
    (void)name;
    (void)fd2;
    return fd1;
}

static inline int sync_accumulate(const char *name, int *fd1, int fd2)
{
    (void)name;
    (void)fd1;
    (void)fd2;
    return 0;
}

#endif
