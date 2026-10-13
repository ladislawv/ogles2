/*
 * ogles2: the TFU (texture formatting unit) of V3D 4.2, for mipmap
 * generation (Mesa's v3dx_tfu.c with for_mipmap, common/v3d_tfu.h).
 */
#ifndef V3D_TFU_H
#define V3D_TFU_H

#include "v3d_types.h"
#include "v3d_device.h"
#include "v3d_texture.h"

/* The texture data formats the TFU filters for mipmaps (Mesa's
 * tfu_supports_tex_format with for_mipmap): 8-bit and 16-bit float ones;
 * not 32-bit floats or depth. */
int v3d_tfu_supports_mipmaps(v3d_u8 hw_type);

/* Levels 1 .. last_level of `tex` (allocated, num_levels > last_level)
 * filtered from its level 0 by the TFU, in place: level 0 is read, not
 * written. `hw_type` is the texture's V3D_TEXTURE_DATA_FORMAT_*. The caller
 * holds the GPU submission lock, has made every job that writes level 0
 * finish and none that samples the texture is still to run, and has
 * cleaned the CPU data cache over level 0. Returns 0 when the levels are
 * written, -1 when the TFU cannot do it (format, layout, size; nothing
 * started), -2 when it did not finish (the TFU was reset). The texture
 * cache is invalidated at the next render job's start, as for any upload. */
int v3d_tfu_generate_mipmaps(V3DDevice* device, V3DTexture* tex, v3d_u8 hw_type, v3d_u8 last_level);

#endif
