/*
 * ogles2: mipmap generation with the TFU (texture formatting unit), new in
 * ogles2's copy of the backend. include/v3d_tfu.h.
 *
 * The job's words are Mesa's (gallium v3dx_tfu.c, V3D_VERSION 42, with
 * for_mipmap: the source and destination are the texture's level 0, IOA's
 * DIMTW keeps level 0 from being written, ICFG's NUMMM is the number of
 * levels to make, and the hardware infers each smaller level's layout --
 * the layout v3d_texture_alloc_mipchain gives them, Mesa's
 * v3d_setup_slices). The registers are written as the Linux kernel's
 * v3d_tfu_job_run writes them, ICFG last (it starts the job), without its
 * interrupt-on-completion bit: completion is polled, as Mesa's simulator
 * does, on V3D_TFU_CS's conversion count.
 */
#include <exec/types.h>
#include "../include/v3d_tfu.h"
#include "v3d_hw.h"
#include "v3d_regs.h"
#include "v3d_debug.h"

/* include/v3d_tfu.h's field values (Mesa's common/v3d_tfu.h, V3D33_*: the
 * V3D 4.2 layout). */
#define TFU_IOA_DIMTW            (1UL << 0)  /* levels after level 0 only */
#define TFU_IOA_FORMAT_SHIFT     3
#define TFU_IOA_FORMAT_LINEARTILE 3          /* + (tiling - LINEARTILE) */
#define TFU_ICFG_NUMMM_SHIFT     5
#define TFU_ICFG_TTYPE_SHIFT     9
#define TFU_ICFG_FORMAT_SHIFT    18
#define TFU_ICFG_FORMAT_LINEARTILE 11        /* + (tiling - LINEARTILE) */
#define TFU_ICFG_OPAD_SHIFT      22

/* Polls of V3D_TFU_CS (each a register read across the PiStorm bridge)
 * before a job counts as stuck: a floor, plus one for every four texels of
 * level 0 (the render wait's scale, v3d_submit_timeout.h). */
#define TFU_POLLS_MIN 100000UL

int v3d_tfu_supports_mipmaps(v3d_u8 hw_type)
{
    switch (hw_type) {
    case V3D_TEXTURE_DATA_FORMAT_R8:
    case V3D_TEXTURE_DATA_FORMAT_R8_SNORM:
    case V3D_TEXTURE_DATA_FORMAT_RG8:
    case V3D_TEXTURE_DATA_FORMAT_RG8_SNORM:
    case V3D_TEXTURE_DATA_FORMAT_RGBA8:
    case V3D_TEXTURE_DATA_FORMAT_RGBA8_SNORM:
    case V3D_TEXTURE_DATA_FORMAT_RGB565:
    case V3D_TEXTURE_DATA_FORMAT_RGBA4:
    case V3D_TEXTURE_DATA_FORMAT_RGB5A1:
    case V3D_TEXTURE_DATA_FORMAT_RGB10A2:
    case V3D_TEXTURE_DATA_FORMAT_R16:
    case V3D_TEXTURE_DATA_FORMAT_R16_SNORM:
    case V3D_TEXTURE_DATA_FORMAT_RG16:
    case V3D_TEXTURE_DATA_FORMAT_RG16_SNORM:
    case V3D_TEXTURE_DATA_FORMAT_RGBA16:
    case V3D_TEXTURE_DATA_FORMAT_RGBA16_SNORM:
    case V3D_TEXTURE_DATA_FORMAT_R16F:
    case V3D_TEXTURE_DATA_FORMAT_RG16F:
    case V3D_TEXTURE_DATA_FORMAT_RGBA16F:
    case V3D_TEXTURE_DATA_FORMAT_R11FG11FB10F:
    case V3D_TEXTURE_DATA_FORMAT_R4:
        return 1;
    default:
        return 0;
    }
}

static v3d_u8 conversions_done(void)
{
    v3d_u32 cs = (v3d_u32)LE32(V3D_TFU_CS);
    return (v3d_u8)((cs >> V3D_TFU_CS_CVTCT_SHIFT) & V3D_TFU_CS_CVTCT_MASK);
}

int v3d_tfu_generate_mipmaps(V3DDevice* device, V3DTexture* tex, v3d_u8 hw_type, v3d_u8 last_level)
{
    v3d_u32 cpp = tex->cpp ? tex->cpp : 4;
    v3d_u32 tiling = tex->levels[0].tiling_format;
    v3d_u32 w = tex->width, h = tex->height;
    v3d_u32 addr, iis = 0, icfg, ioa, polls, limit;
    v3d_u8 before;
    (void)device;

    if (!tex->texture_mem.hostptr || last_level < 1 || last_level >= tex->num_levels ||
        !v3d_tfu_supports_mipmaps(hw_type) || w < 1 || h < 1 || w > 16383 || h > 16383 ||
        tiling == V3D_MEMORY_FORMAT_RASTER || tiling > V3D_MEMORY_FORMAT_UIF_XOR)
        return -1;

    /* Level 0 where the texture records point the TMU (v3d_texture_emit_state):
     * GPU and CPU addresses are the same memory. */
    addr = (v3d_u32)V3D_ALIGN_UP((v3d_uintptr)tex->texture_mem.hostptr, 256) + tex->levels[0].offset;

    if (tiling == V3D_MEMORY_FORMAT_UIF_NO_XOR || tiling == V3D_MEMORY_FORMAT_UIF_XOR)
        iis = tex->levels[0].padded_h / (2 * v3d_utile_height((int)cpp));

    icfg = ((TFU_ICFG_FORMAT_LINEARTILE + (tiling - V3D_MEMORY_FORMAT_LINEARTILE)) << TFU_ICFG_FORMAT_SHIFT) |
           ((v3d_u32)hw_type << TFU_ICFG_TTYPE_SHIFT) |
           ((v3d_u32)last_level << TFU_ICFG_NUMMM_SHIFT);
    /* OPAD: UIF blocks beyond the height's own (Mesa: supplied whenever the
     * base level is UIF; the smaller levels' padding is inferred). */
    if (tiling == V3D_MEMORY_FORMAT_UIF_NO_XOR || tiling == V3D_MEMORY_FORMAT_UIF_XOR) {
        v3d_u32 uif_block_h = 2 * v3d_utile_height((int)cpp);
        v3d_u32 implicit = V3D_ALIGN_UP(h, uif_block_h);
        if (tex->levels[0].padded_h < implicit)
            return -1;
        icfg |= ((tex->levels[0].padded_h - implicit) / uif_block_h) << TFU_ICFG_OPAD_SHIFT;
    }
    ioa = addr | TFU_IOA_DIMTW |
          ((TFU_IOA_FORMAT_LINEARTILE + (tiling - V3D_MEMORY_FORMAT_LINEARTILE)) << TFU_IOA_FORMAT_SHIFT);

    /* A job of another kind of user would be ours to wait for; the lock
     * keeps other programs out, so the unit is normally idle. */
    for (polls = 0; ((v3d_u32)LE32(V3D_TFU_CS) & V3D_TFU_CS_BUSY) && polls < TFU_POLLS_MIN; polls++)
        ;
    before = conversions_done();

    V3D_TFU_IIA = LE32(addr);
    V3D_TFU_IIS = LE32(iis);
    V3D_TFU_ICA = LE32(0);
    V3D_TFU_IUA = LE32(0);
    V3D_TFU_IOA = LE32(ioa);
    V3D_TFU_IOS = LE32((h << 16) | w);
    V3D_TFU_COEF0 = LE32(0);
    V3D_TFU_ICFG = LE32(icfg); /* starts it */

    limit = TFU_POLLS_MIN + ((w * h) >> 2);
    for (polls = 0; conversions_done() == before; polls++) {
        if (polls > limit) {
            D(("v3d_tfu_generate_mipmaps: no completion after %lu polls, resetting the TFU\n", (ULONG)polls));
            V3D_TFU_CS = LE32(V3D_TFU_CS_TFURST);
            return -2;
        }
    }
    return 0;
}
