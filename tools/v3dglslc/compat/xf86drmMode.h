/* Stand-in for libdrm's <xf86drmMode.h> (KMS). The V3D driver includes it;
 * shader compilation uses no display mode setting. */
#ifndef OGLES2_COMPAT_XF86DRMMODE_H
#define OGLES2_COMPAT_XF86DRMMODE_H
#include "xf86drm.h"
#endif
