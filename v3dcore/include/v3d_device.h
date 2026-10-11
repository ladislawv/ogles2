/*
 * (C) 2025-2026 Dennis van der Boon
 */

#ifndef V3D_DEVICE_H
#define V3D_DEVICE_H

#include "v3d_types.h"

/*
 * V3DDevice -- derived from an earlier library by the same author. It keeps
 * only the fields v3d_init/v3d_mem_alloc/v3d_mem_free/v3d_free/
 * v3d_qpu_active use (sysbase, dosbase, BytesAllocated, deviceInfo). The
 * rest is not carried over; that includes the Warp3D driver registration
 * (struct Library lib, W3D_Driver driver/owndriver/end), which MiniGLV3D
 * does not need since it links this in directly rather than registering
 * as a loadable Warp3D driver.
 */

struct v3d_device_info {
    unsigned char ver;              /* simple V3D version: major*10 + minor */
    unsigned char rev;              /* ogles2: HUB_IDENT3 revision, as Mesa (4.2.14 on a Pi 4) */
    int vpm_size;                    /* VPM size, in bytes */
    int qpu_count;                   /* NSLC * QUPS from the core's IDENT registers */
    char has_accumulators;           /* whether the hw has accumulator registers */
    /* ogles2: the clipper's XY granularity (256 on V3D 4.2), for the
     * viewport uniforms and CLIPPER_XY_SCALING, as Mesa's devinfo. */
    unsigned short clipper_xy_granularity;
};

typedef struct V3DDevice {
    struct ExecBase* sysbase;
    struct DOSBase*  dosbase;
    v3d_u32 BytesAllocated;
    struct v3d_device_info deviceInfo;
    /* ogles2: TRUE if the GPU reaches [addr, addr+size) (the device tree's
     * memory ranges, plan 7.3); NULL accepts everything. v3d_mem_alloc
     * refuses blocks it rejects. */
    int (*gpu_reachable)(unsigned long addr, unsigned long size);
    /* ogles2: mailbox state, formerly file statics in v3d_device.c. */
    APTR devicetreebase;
    ULONG mailbox;                   /* CPU address of the VideoCore mailbox */
    v3d_u32 mbreq_storage[32 + 4];
    v3d_u32 *mbreq;                  /* 16-byte aligned inside mbreq_storage */
    unsigned wait_num;
} V3DDevice;

int  v3d_init(V3DDevice* device);
void v3d_free(V3DDevice* device);
int  v3d_mem_alloc(V3DDevice* device, v3d_mem* m, unsigned size);
void v3d_mem_free(V3DDevice* device, v3d_mem* m);
/* ogles2: reset the V3D core (v3d_device.c); 0 if it answers afterwards. */
int  v3d_gpu_reset(V3DDevice* device);
int  v3d_qpu_active(V3DDevice* device);

#endif /* V3D_DEVICE_H */
