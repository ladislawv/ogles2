# M7: the five optional features dEQP-GLES2 found missing

dEQP-GLES2 reports some cases as "not supported" because ogles2.library does
not offer an optional feature. That is allowed for conformance, but all five
features exist on V3D 4.2. This plan adds them, in order of size. Each step
ends with its dEQP group passing on the A1200 and the earlier tests
unchanged.

| # | Feature | dEQP cases now not supported | Where the work is | Size |
|---|---|---|---|---|
| 1 | `GL_EXT_debug_marker` | 3 (`debug_marker`) | library: 3 entry points | small |
| 2 | 32 combined texture units | 8 (`uniform_api`) | library and compiler limits | small |
| 3 | S3TC: `GL_EXT_texture_compression_dxt1`, `GL_EXT_texture_compression_s3tc` | 9 (`negative_api.texture`), and real programs | library: native BC1-BC3 textures | medium |
| 4 | `GL_EXT_clip_control` | 8 (`clip_control`) | library; compiler only if the hardware probe says so | medium |
| 5 | `GL_EXT_draw_buffers` | 54 (`fbo.completeness.attachment_combinations`) and others | library, job setup, compiler variants | large |

Also: the 16 `multisample` cases need no new code. They were not supported
only because the runs had no multisampled default framebuffer. Package
`m7-1` runs them with `Run-M1 LIST=multisample MSAA=1`.

## Shared ground

- **Library version 1.3.** Steps 1, 4 and 5 add entry points:
  - `glInsertEventMarkerEXT`, `glPushGroupMarkerEXT`, `glPopGroupMarkerEXT`;
  - `glClipControlEXT`;
  - `glDrawBuffersEXT`.

  They are listed as `command` lines in `gen/api.txt` and appended to
  `gen/abi_order.txt` (append-only). The dispatch table grows, so the
  library version goes up; the ABI version does not (§6.7). Programs reach
  them through `aglGetProcAddress`, as SDL and gl4es do.
- **One compiler rebuild** for steps 2 and 5, which both change the limits
  `v3dglslc` gives Mesa: 32 combined units, 4 draw buffers, and the draw-buffer
  variants. It invalidates the shader caches once:
  - the package caches (cleared);
  - Quake III's shipped cache, compiled again on the A1200.

  Step 4 changes the compiler only if its hardware probe fails.
- **Compiler id.** The library recognises a compiler by its file size
  (`ogles2_compiler_id`). Two builds of the same size share an id, and then
  stale compile records (`.v3ds`) are accepted. This happened during M7
  (19,538,220 bytes twice). Before this work the id becomes the size plus a
  build stamp, a version string compiled into `v3dglslc` and read from the
  file's tail. This is done first, because every later step relies on it.
- **Extensions string and Appendix B.3:** each step adds its extension to
  `GL_EXTENSIONS` and to the appendix's table when it is done.
- **Tests:** each step has an ogles2 test (package `m6-6` grows, or a new
  `m7-2`) besides dEQP. The earlier packages run again at the end: `m5-1`, the
  corpus `m2-5`, `m6-6` (compiles on the A1200), `m6-10` (MSAA) and Quake III
  `m6-5`.

## 1. `GL_EXT_debug_marker`

- **What:** markers for debugging tools. `glInsertEventMarkerEXT` marks a
  point; `glPushGroupMarkerEXT` and `glPopGroupMarkerEXT` bracket a group.
  The extension only requires that the calls be accepted.
- **Design:** the markers go into ogles2's log when tracing is on (the
  `ogles2_trace` path), indented by group depth, so a log of a frame shows
  the program's own structure. A pop without a push is ignored, as the
  extension says. The depth is kept per context, at most 64 levels.
- **Files:** `gen/api.txt`, `gen/abi_order.txt`, `ogles2/state.c` (or a
  small `debug.c`), and the extensions string.
- **Done when:** dEQP `debug_marker` (3 cases) passes, and markers appear
  in a traced log.

## 2. 32 combined texture units

- **What:** GLES2 texture units are binding points. A stage has at most 16
  samplers, but the program as a whole may use units 0-31 across its stages
  (`GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS`). Mesa's v3d says 96 combined; 32 is
  enough for every dEQP case and costs less memory.
- **Design:**
  - Split `OGLES2_MAX_TEXTURE_UNITS` (now 16, used for both) into
    `OGLES2_MAX_TEXTURE_UNITS` = 32, the binding points, and
    `OGLES2_MAX_STAGE_SAMPLERS` = 16, a stage's texture indices.
  - Bindings per unit and the `glActiveTexture` range become 32.
  - `glUniform1i`'s sampler range becomes 0..31.
  - `draw.c`'s texture state arrays stay per stage index (16).
  - Queries: `GL_MAX_TEXTURE_IMAGE_UNITS` 16,
    `GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS` 16,
    `GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS` 32.
  - Compiler: `ctx->Const.MaxCombinedTextureImageUnits` 32, so
    `gl_MaxCombinedTextureImageUnits` agrees.
- **Files:** `ogles2/ogles2_internal.h`, `texture.c`, `program.c`,
  `draw.c`, `tools/v3dglslc/v3dglslc.c`.
- **Done when:** the 8 `uniform_api` cases pass, `builtin_variable` still
  passes, and `m2_texture` is unchanged.

## 3. S3TC (BC1-BC3)

- **What:** `GL_EXT_texture_compression_dxt1` (DXT1 RGB and RGBA),
  `GL_EXT_texture_compression_s3tc` (also DXT3 and DXT5), and
  `GL_ANGLE_texture_compression_dxt3` and `_dxt5`, which are the GLES names
  some programs look for.
  - The patents have expired.
  - V3D's texture unit samples them natively: texture data formats BC1, BC2
    and BC3 (48-50), which Mesa's v3d format table maps DXT1, DXT3 and DXT5
    to.
  - Unlike ETC1, which ogles2 decodes to RGBA8 on the CPU, they stay
    compressed in GPU memory: 4x less memory than RGBA8 for DXT1, 2x less
    for DXT3/5.
  - `glCompressedTexSubImage2D` is allowed on 4x4 block boundaries, which is
    what the 9 `negative_api` cases need.
- **Design:**
  - A level's RAM copy holds the blocks as given, in a new storage class
    `CLASS_BLOCKS`. The GPU copy is laid out like an uncompressed texture of
    (w/4)x(h/4) "pixels" of 8 or 16 bytes, as Mesa's `v3d_resource` does with
    a compressed format's block size. ogles2's tiling code already handles 8-
    and 16-byte pixels.
  - `storage_of` gets the three hardware formats and their swizzles from
    Mesa's table: DXT1 RGB is XYZ1, the others XYZW.
  - `glCompressedTexImage2D` checks the size,
    `ceil(w/4)*ceil(h/4)*block bytes`, else `GL_INVALID_VALUE`.
  - `glCompressedTexSubImage2D` needs offsets that are multiples of 4, and a
    width and height that are multiples of 4 or reach the level's edge, else
    `GL_INVALID_OPERATION`. It writes the RAM copy's blocks and marks the
    level for upload as `glTexSubImage2D` does.
  - Not allowed on these textures, each with `GL_INVALID_OPERATION`:
    - `glTexSubImage2D` (uncompressed data);
    - `glCopyTex(Sub)Image2D`;
    - `glGenerateMipmap`;
    - use as a framebuffer attachment (not colour-renderable).
  - `GL_COMPRESSED_TEXTURE_FORMATS` lists ETC1 and the four DXT formats.
  - A mip level smaller than 4x4 is still one block.
- **Check before coding:** V3D 4.2 samples BC formats from UIF-tiled memory
  with 4x4 blocks as the "pixels", as Mesa does. A first A1200 test draws a
  known DXT1 texture (`m7-2`): each block's two colours and its 2-bit
  indices, and an alpha-punch block.
- **Files:** `ogles2/texture.c`, `v3dcore` (formats, block-aligned level
  sizes), and the extensions string.
- **Done when:**
  - the 9 `negative_api` cases pass;
  - `texture.format` and `texture.size` are unchanged;
  - the new test samples DXT1, DXT3 and DXT5 to the colours the S3TC
    specification gives (exact for DXT1; DXT3/5 alpha to within 1);
  - optional: Quake III's `r_ext_compressed_textures`. In GLES2 it needs
    pre-compressed data, so it may stay unused.

## 4. `GL_EXT_clip_control`

- **What:** `glClipControlEXT(origin, depth)`. It needs two things:
  - the origin: lower left as now, or upper left, which turns the picture
    upside down in window space;
  - the depth mode: clip-space z from -w..w (`NEGATIVE_ONE_TO_ONE_EXT`) or
    0..w (`ZERO_TO_ONE_EXT`).
- **The hardware question:** V3D 4.2 has no 0..1 Z-clip mode. It exists from
  V3D 7.1 (Mesa's `clip_halfz` is set only for `ver >= 71`, so Mesa does not
  offer the extension on a Pi 4). What 4.2 has is
  `CLIPPER_Z_MIN_MAX_CLIPPING_PLANES` in window-space depth, which Mesa
  fills from the viewport (`util_viewport_zmin_zmax`).
- **Design A, runtime only, preferred:**
  - for `ZERO_TO_ONE`, the viewport's Z scale and offset become
    `(f - n, n)` instead of `((f - n)/2, (f + n)/2)`;
  - the clipping planes become `[n, f]` in window depth, so clip-space
    z < 0 lands below n and is clipped.
- **The probe decides:** an A1200 test draws a triangle crossing clip-space
  z = 0 in `ZERO_TO_ONE` mode and reads which part is drawn.
  - **The planes clip:** the part with z < 0 is gone, and design A works.
  - **The planes only clamp:** that part is drawn at depth n. Then design B
    applies.
- **Design B, the fallback:** Mesa's `nir_lower_clip_halfz_dynamic` in the
  vertex shader, reading the mode from a uniform (a new uniform recipe type,
  like the PiStorm3D viewport ones). This is a compiler change: one FMA per
  vertex, and every program's binary changes.
- **Upper-left origin, in either design:**
  - viewport Y scale negated;
  - `glFrontFace` reversed for culling;
  - `gl_FragCoord.y` through the existing `FbWposYTransform` state
    variable, which already handles ogles2's flipped default framebuffer;
  - `dFdy` and `gl_PointCoord` sign as for that flip.
- **Queries:** `GL_CLIP_ORIGIN_EXT` and `GL_CLIP_DEPTH_MODE_EXT`.
- **Files:** `ogles2/state.c`, `draw.c` (the viewport group), `frame.c`;
  for design B also `tools/v3dglslc` and a Mesa patch.
- **Done when:**
  - dEQP `clip_control` (8 cases) passes;
  - `m4_depth` and `m1_draw` are unchanged;
  - Quake III renders as before (it never calls the function).

## 5. `GL_EXT_draw_buffers`

- **What:**
  - Up to 4 colour attachments (`GL_COLOR_ATTACHMENT0..3`; V3D 4.2 has 4
    render targets).
  - `glDrawBuffersEXT` chooses which outputs go where.
  - Shaders write `gl_FragData[0..3]` after
    `#extension GL_EXT_draw_buffers : enable`.
  - It brings the 54 `fbo.completeness.attachment_combinations` cases, and
    the `gl_MaxDrawBuffers` and `fragdata` cases with their values for it.
- **Compiler:**
  - `MaxDrawBuffers` back to 4. Mesa enables `EXT_draw_buffers` for GLES2
    whenever there is more than one draw buffer.
  - **Check first:** with 4, `gl_FragData[1]` without the `#extension`
    directive must still fail to compile (`fragdata.invalid_assign_to_1`).
    If Mesa lets it through, a small parser patch makes the index depend on
    the directive.
  - **Fragment variants:** Mesa's fragment key has the colour buffers in use
    (`cbufs`) and per-buffer format bits. Today `v3dglslc`'s key-state draws
    make variants for one RGBA8 buffer. For a program whose fragment shader
    writes `gl_FragData[i > 0]`, they also make variants for `cbufs` 0x3,
    0x7 and 0xF, with 8-bit normalised buffers: RGBA8, RGB565, RGBA4 and
    RGB5_A1 all use the tile buffer's 8-bit internal type. The `.v3dp`
    format already carries a variant's key, and the runtime chooses by the
    attachments in use.
- **Library:**
  - `fbo.c`: three more attachment points. Completeness:
    - every colour attachment the same size, as GLES2 requires of all
      attachments;
    - `GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER` and `_READ_BUFFER` do not
      exist in GLES2 with this extension.
  - `glDrawBuffersEXT`:
    - the default framebuffer takes only `GL_BACK`;
    - a framebuffer object takes `GL_COLOR_ATTACHMENTi` in position i, or
      `GL_NONE`;
    - the state is the framebuffer's own.
  - Queries: `GL_MAX_DRAW_BUFFERS_EXT` 4, `GL_MAX_COLOR_ATTACHMENTS_EXT` 4,
    `GL_DRAW_BUFFERi_EXT`.
  - `job.c`: a job with n render targets
    (`TILE_RENDERING_MODE_CFG_COLOR` per target, with its internal type and
    bytes per pixel). The tile size follows Mesa's `v3d_choose_tile_size`:
    with 4 targets of 32 bits, 32x32 tiles, as for MSAA. Per target, a
    load (unless cleared) and a store into its own attachment's layout.
  - `glClear` clears every draw buffer. Partial clears draw the rectangle
    with the colour written to all outputs.
  - Blending (`BLEND_CFG`) already names all four targets. The colour write
    mask applies to every target, as GLES2 has one mask.
  - `glReadPixels` reads attachment 0, since GLES2 has no
    `glReadBuffer`.
  - Discard (`EXT_discard_framebuffer`) covers every attachment.
- **Tests:**
  - an ogles2 test (`m7-2`) writes four colours to four textures in one
    draw and samples each back;
  - clear, blend and partial clear per target;
  - the attachment-combination completeness rules;
  - dEQP `fbo` and `shaders.fragdata` (now with 4).
- **Risk:** the biggest step. The job code's single-target assumptions are
  scattered: loads, stores, clears, tile size, the depth save across a
  mid-frame flush. This goes last, after steps 1-4 are done and committed.

## Order and checkpoints

1. **Compiler id:** the build stamp (shared ground).
2. **Step 1:** debug markers. Library 1.3, the new entry points' mechanism
   checked end to end.
3. **Step 2:** 32 combined units.
4. **Step 3:** S3TC, with its A1200 sampling test first.
5. **Step 4:** clip control, with its A1200 Z-plane probe first.
6. **Step 5:** draw buffers, the compiler part together with step 2's limit
   when that is rebuilt.
7. **The full dEQP-GLES2 list** on the final build, `multisample` with
   `MSAA=1`, the earlier packages, and Quake III with its shader cache
   compiled again.
8. **Plan and status (§13):** the extensions in Appendix B.3, the
   conformance results with only the cube-map deviation left.

A step is committed when its own tests and dEQP group pass.
