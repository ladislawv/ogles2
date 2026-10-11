/*
 * The library's V3D device (v3dcore's V3DDevice), initialised once, at the
 * first context creation: opening the library never touches the GPU.
 *
 * Not shut down at expunge: v3dcore's v3d_free() switches the QPUs off
 * through the firmware, which would pull the GPU from under a minigl.library
 * still in use (plan decision D3: until the shared core of M6, the two
 * libraries must not draw at the same time, but either may be loaded).
 */
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "ogles2_internal.h"
#include <exec/memory.h>
#include "v3d_device.h"
#include "v3d_context.h"
#include "v3d_frame.h"
#include "v3d_gpu_lock.h"
#include "v3d_hw.h"
#include "v3d_regs.h"
#include "devicetree_protos.h"

/* ---- a clock for the timing statistics ----
 * The Pi's system timer: a free-running 1 MHz counter (CLO, peripheral
 * offset 0x3004), in the peripheral window v3dcore's registers use. One
 * read, on the Pi's side: timer.device's E-clock reads the Amiga's CIAs
 * across the PiStorm bus, which cost about 25 microseconds a call. */
#define SYSTIMER_CLO (*(volatile ULONG *)(0xF2000000 + 0x3004))

ULONG ogles2_clock_us(void)
{
    return LE32(SYSTIMER_CLO);
}

static V3DDevice device;

ULONG ogles2_device_bytes(void)
{
    return (ULONG)device.BytesAllocated;
}
static struct SignalSemaphore *gpu_lock; /* public, shared with other GPU clients */
static LONG state;          /* 0 not tried, 1 ready, < 0 v3d_init's error */
static char renderer[96];

struct fmt { char *p; ULONG left; };

static void put_char(register UBYTE c __asm("d0"), register struct fmt *f __asm("a3"))
{
    if (f->left > 1) {
        *f->p++ = (char)c;
        f->left--;
    } else if (f->left == 1) {
        *f->p = 0;
    }
}

static const char *init_error(LONG e)
{
    switch (e) {
    case -1: return "no /soc key in the device tree";
    case -2: return "mailbox alias key missing";
    case -3: return "no mailbox alias";
    case -4: return "no /aliases key";
    case -5: return "no devicetree.resource (not running on Emu68?)";
    case -6: return "V3D did not start (IDENT0 wrong)";
    default: return "unknown";
    }
}

/* Plan section 7.1: until minigl.library joins the submission lock (M6), the
 * two must not draw at the same time. Warn when it is open. */
static void warn_if_minigl_open(void)
{
    struct Library *l;
    UWORD opens = 0;

    Forbid();
    l = (struct Library *)FindName(&SysBase->LibList, (STRPTR) "minigl.library");
    if (l)
        opens = l->lib_OpenCnt;
    Permit();
    if (opens)
        ogles2_log("warning: minigl.library is open (%ld users); ogles2 and MiniGL programs "
                   "must not draw at the same time", (LONG)opens);
}

/* ---- the RAM the GPU can reach (plan sections 2.3 and 7.3) ----
 * The device tree's memory node lists it, in the Pi's physical addresses,
 * which are the 68k's under Emu68 and the GPU's bus addresses. Read once at
 * device init; every exec memory region is checked against it and logged. */
#define MAX_RAM_RANGES 8
static ULONG ram_lo[MAX_RAM_RANGES], ram_hi[MAX_RAM_RANGES]; /* [lo, hi) */
static int ram_ranges;

static ULONG cells(APTR DeviceTreeBase, APTR key, const char *name, ULONG dflt)
{
    APTR prop = DT_FindProperty(key, name);
    const ULONG *v = prop ? DT_GetPropValue(prop) : 0;
    return v ? v[0] : dflt;
}

static void read_ram_ranges(void)
{
    APTR DeviceTreeBase = device.devicetreebase;
    APTR root, child = 0;
    ULONG ac, sc;
    if (!DeviceTreeBase || !(root = DT_OpenKey("/")))
        return;
    ac = cells(DeviceTreeBase, root, "#address-cells", 2);
    sc = cells(DeviceTreeBase, root, "#size-cells", 1);
    while ((child = DT_GetChild(root, child))) {
        APTR prop = DT_FindProperty(child, "device_type");
        const char *type = prop ? DT_GetPropValue(prop) : 0;
        const ULONG *reg;
        ULONG len, n, i;
        if (!type || type[0] != 'm' || type[1] != 'e' || type[2] != 'm' || type[3] != 'o' ||
            type[4] != 'r' || type[5] != 'y' || type[6] != 0)
            continue;
        if (!(prop = DT_FindProperty(child, "reg")) || !(reg = DT_GetPropValue(prop)))
            continue;
        len = DT_GetPropLen(prop) / 4;
        n = len / (ac + sc);
        for (i = 0; i < n && ram_ranges < MAX_RAM_RANGES; i++) {
            const ULONG *e = reg + i * (ac + sc);
            ULONG hi_addr = ac > 1 ? e[ac - 2] : 0, addr = e[ac - 1];
            ULONG hi_size = sc > 1 ? e[ac + sc - 2] : 0, size = e[ac + sc - 1];
            if (hi_addr) /* above 4 GB: out of the 68k's reach */
                continue;
            if (hi_size || addr + size < addr)
                size = 0UL - addr;
            ram_lo[ram_ranges] = addr;
            ram_hi[ram_ranges] = addr + size;
            ogles2_log("device: GPU-visible RAM (device tree %s): 0x%08lx-0x%08lx", (ULONG)"memory",
                       addr, addr + size - 1);
            ram_ranges++;
        }
    }
    DT_CloseKey(root);
}

static int gpu_reachable_hook(unsigned long addr, unsigned long size)
{
    if (ogles2_device_gpu_reachable(addr, size))
        return 1;
    ogles2_log("device: refused a %lu-byte block at 0x%08lx: outside the GPU-visible RAM", size,
               addr);
    return 0;
}

/* TRUE if [addr, addr+size) lies in RAM the GPU reaches. Without device-tree
 * ranges nothing can be checked: TRUE. */
BOOL ogles2_device_gpu_reachable(ULONG addr, ULONG size)
{
    int i;
    if (!ram_ranges)
        return TRUE;
    for (i = 0; i < ram_ranges; i++)
        if (addr >= ram_lo[i] && addr + size <= ram_hi[i] && addr + size >= addr)
            return TRUE;
    return FALSE;
}

/* Logs each exec memory region and whether the GPU can reach it. */
static void check_memory_regions(void)
{
    struct MemHeader *mh;
    struct { ULONG lo, hi, attr; char name[24]; } r[16];
    int n = 0, i, k;
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head; mh->mh_Node.ln_Succ && n < 16;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ, n++) {
        const char *nm = mh->mh_Node.ln_Name ? mh->mh_Node.ln_Name : "";
        r[n].lo = (ULONG)mh->mh_Lower;
        r[n].hi = (ULONG)mh->mh_Upper;
        r[n].attr = mh->mh_Attributes;
        for (k = 0; k < 23 && nm[k]; k++)
            r[n].name[k] = nm[k];
        r[n].name[k] = 0;
    }
    Permit();
    for (i = 0; i < n; i++)
        ogles2_log("device: memory region \"%s\" 0x%08lx-0x%08lx%s%s: %s", (ULONG)r[i].name, r[i].lo,
                   r[i].hi - 1, (ULONG)((r[i].attr & MEMF_CHIP) ? " chip" : ""),
                   (ULONG)((r[i].attr & MEMF_FAST) ? " fast" : ""),
                   (ULONG)(ogles2_device_gpu_reachable(r[i].lo, r[i].hi - r[i].lo)
                               ? "the GPU reaches it"
                               : "OUTSIDE the GPU-visible RAM"));
}

struct SignalSemaphore *ogles2_device_gpu_lock(void)
{
    return gpu_lock;
}

static void reset_if_stopped(void);

BOOL ogles2_device_open(void)
{
    ObtainSemaphore(&OGLES2LibBase->lock);
    warn_if_minigl_open();
    if (!gpu_lock) {
        gpu_lock = v3d_gpu_lock_get();
        ogles2_log("GPU submission lock \"%s\" at 0x%08lx", (ULONG)V3D_GPU_LOCK_NAME,
                   (ULONG)gpu_lock);
    }
    if (state == 0) {
        LONG ret;
        device.sysbase = SysBase;
        device.dosbase = (struct DOSBase *)DOSBase;
        ret = v3d_init(&device);
        if (ret == 0) {
            const struct v3d_device_info *i = &device.deviceInfo;
            struct fmt f;
            LONG args[5];
            args[0] = i->ver / 10;
            args[1] = i->ver % 10;
            args[2] = i->rev;
            args[3] = i->qpu_count;
            args[4] = i->vpm_size / 1024;
            f.p = renderer;
            f.left = sizeof(renderer);
            RawDoFmt((STRPTR) "V3D %ld.%ld.%ld (PiStorm), %ld QPUs, %ld KB VPM", args,
                     (void (*)())put_char, &f);
            put_char(0, &f);
            ogles2_log("device: %s, clipper granularity %ld", (ULONG)renderer,
                       (LONG)i->clipper_xy_granularity);
            read_ram_ranges();
            check_memory_regions();
            device.gpu_reachable = gpu_reachable_hook;
            state = 1;
            reset_if_stopped();
        } else {
            ogles2_log("device: v3d_init failed (%ld: %s)", ret, (ULONG)init_error(ret));
            state = ret;
        }
    }
    ReleaseSemaphore(&OGLES2LibBase->lock);
    return state == 1;
}

/* The GPU did not finish a job (plan 9 M4): reset it, so the next job
 * finds it idle. The caller holds the GPU submission lock. */
void ogles2_device_reset(const char *why)
{
    int r = v3d_gpu_reset(&device);
    ogles2_log("device: GPU reset after %s: %s", (ULONG)why, (ULONG)(r == 0 ? "the core answers" : "NO ANSWER"));
}

/* A GPU left stopped by an earlier program (control list thread 0 still
 * running, its address standing still) is reset before first use. Only
 * with the submission lock: a client rendering now holds it. */
static void reset_if_stopped(void)
{
    ULONG cs, ca1, ca2;
    if (!v3d_gpu_lock(gpu_lock, 200))
        return;
    cs = LE32(V3D_CT0CS);
    ca1 = V3D_CT0CA;
    Delay(2);
    ca2 = V3D_CT0CA;
    if ((cs & 0x20) && ca1 == ca2 && LE32(V3D_CT0CA) != LE32(V3D_CT0EA)) {
        ogles2_log("device: binning thread stopped at 0x%08lx (CT0CS 0x%08lx), left by an earlier program",
                   (ULONG)LE32(ca1), cs);
        ogles2_device_reset("finding it stopped");
    }
    v3d_gpu_unlock(gpu_lock);
}

BOOL ogles2_device_ready(void)
{
    return state == 1;
}

V3DDevice *ogles2_device(void)
{
    return &device;
}

V3DContext *ogles2_device_context_new(UWORD w, UWORD h, int depth_bits)
{
    V3DContext *v = AllocVec(sizeof(*v), MEMF_ANY | MEMF_CLEAR);
    int ret;
    if (!v)
        return 0;
    /* v3d_context_init reads the depth format from a global (it is a
     * MiniGL-era setting): set and use it under the library lock. */
    ObtainSemaphore(&OGLES2LibBase->lock);
    g_v3d_requested_zbuffer_bits = depth_bits == 16 ? 16 : 32;
    ret = v3d_context_init(v, &device, w, h);
    ReleaseSemaphore(&OGLES2LibBase->lock);
    if (ret != 0) {
        ogles2_log("device: v3d_context_init(%ldx%ld) failed (%ld): out of memory", (ULONG)w,
                   (ULONG)h, (LONG)ret);
        FreeVec(v);
        return 0;
    }
    ogles2_log("device: context pools for %ldx%ld, D%ld%s, %lu bytes of GPU memory in use", (ULONG)w,
               (ULONG)h, (LONG)v->zbuffer_bits, (ULONG)(v->zbuffer_bits == 32 ? "F" : ""),
               (ULONG)device.BytesAllocated);
    return v;
}

BOOL ogles2_device_context_resize(V3DContext *v, UWORD w, UWORD h)
{
    UWORD ow = v->width, oh = v->height;
    int ret = v3d_context_resize(v, &device, w, h);
    if (ret == 0) {
        ogles2_log("device: context pools for %ldx%ld, %lu bytes of GPU memory in use", (ULONG)w,
                   (ULONG)h, (ULONG)device.BytesAllocated);
        return TRUE;
    }
    ogles2_log("device: v3d_context_resize(%ldx%ld) failed (%ld): out of memory", (ULONG)w, (ULONG)h,
               (LONG)ret);
    if (v3d_context_resize(v, &device, ow, oh) != 0)
        ogles2_log("device: and back to %ldx%ld failed too", (ULONG)ow, (ULONG)oh);
    return FALSE;
}

void ogles2_device_context_free(V3DContext *v)
{
    if (!v)
        return;
    v3d_context_free(&device, v);
    FreeVec(v);
    ogles2_log("device: context freed, %lu bytes of GPU memory still in use",
               (ULONG)device.BytesAllocated);
}

const char *ogles2_device_renderer(void)
{
    return state == 1 ? renderer : "V3D 4.2 (PiStorm, not initialised)";
}
