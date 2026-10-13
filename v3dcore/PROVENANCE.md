# v3dcore: provenance and changes

`hw/` and `include/` are PiStorm3D's V3D backend (`backend/`), copied from branch `PiStorm3D`
of `SteffenHaeuser/MiniGL_Library_68k` at commit `01f5a3e45fc7` (2026-10-08, "Updated to V29.1"),
the source of the shipped `minigl.library` 29.1. Line ends converted to LF; nothing else changed
in the copy.

Licence: Hyperion MiniGL Open Source License (`Licence.txt`, from the same commit), except the
parts whose headers carry their own notices (Macoy Madson's port of Mesa's V3D code, Broadcom and
VMware notices; see `../licences/License_V3D_ThirdParty.txt`). Those notices stay in the files.

It is ogles2's own copy (plan section 4.2, decision D3): minigl.library is not touched, and
sharing one core between the two libraries is deferred to milestone M6.

## Changes since the copy

Each change is marked in the source with `ogles2:`.

| Milestone | File | Change | Why |
|---|---|---|---|
| M0 | `hw/v3d_regs.h` | `V3D_HUB_IDENT3` | Revision field, as Mesa reads it |
| M0 | `include/v3d_device.h`, `hw/v3d_device.c` | Mailbox state (`DeviceTreeBase`, mailbox address, request buffer, `wait_num`) moved from file statics into `V3DDevice` | Plan section 4.3 item 4: device state in the device object |
| M0 | `hw/v3d_gpu_lock.c`, `include/v3d_gpu_lock.h` (new) | Public `"V3D 4.2 submit"` semaphore, obtained with a timeout | Plan section 7.1, phase 1: GPU clients must not interleave jobs |
| M0 | `hw/v3d_device.c` (`v3d_read_info`) | Fills `vpm_size`, `qpu_count`, `rev` and `clipper_xy_granularity` from the identity registers, as Mesa's `v3d_get_device_info` | Needed by the VPM configuration (plan 7.9), spill sizing (8.6) and the viewport uniforms; nothing set them before |

| M1 | `include/v3d_frame.h`, `hw/v3d_frame.c` | The retire list, job sequence numbers and the per-job spill count moved from file statics into `V3DFrame` (one per context); `v3d_frame_begin` initialises them. No call site changed | Plan section 4.3 item 4: two contexts must not retire each other's blocks |
| M1 | `include/v3d_device.h`, `hw/v3d_mem_allocvec.c` | `v3d_mem_alloc` asks for `MEMF_FAST` (it asked only for `MEMF_PUBLIC`) and gives back a block that `V3DDevice.gpu_reachable` rejects; ogles2 sets the hook from the device tree's memory ranges | Plan section 7.3: chip RAM and fast RAM outside those ranges (the A1200 has 512 KB at `0x00C00000`) must never become GPU memory |
| M4 | `hw/v3d_device.c`, `include/v3d_device.h` | `v3d_gpu_reset`: the V3D core reset through `PM_GRAFX`'s reset line, with the AXI bridges stopped around it (Linux's bcm2835-power sequence) | A stopped GPU stayed stopped across Amiga reboots, since they do not reset the Pi (A1200 runs `ogles2-m4-1`) |
| M4 | `include/v3d_texture.h`, `hw/v3d_texture.c` | `V3DTexture.cpp`; `v3d_texture_alloc_cpp` (the layout for 1, 2 or 4 bytes per texel; `v3d_texture_alloc` calls it with 4, the mip chain follows `cpp`); `v3d_texture_store`, an upload of any box with Mesa's stride and padded-height convention | Native texture formats (plan 7.6), and non-power-of-two sizes laid out as the hardware reads them |
| M5 | `include/v3d_texture.h`, `hw/v3d_texture.c` | `v3d_texture_load`: reads a box of a level back, the inverse of `v3d_texture_store` | Render to texture: the RAM copy of a level the GPU drew into, and `glReadPixels` from a framebuffer object (plan 7.7) |
| M7 | `include/v3d_texture.h`, `hw/v3d_texture.c` | `V3DTexture.block`, `v3d_texture_alloc_blocks`: every level laid out as an image of its 4x4 blocks (8 or 16 bytes), each level's size divided by the block size rounding up, as Mesa's `v3d_setup_slices` | S3TC textures (BC1-BC3) stay compressed in GPU memory (`docs/M7_EXTENSIONS_PLAN.md` step 3) |
| M7 | `include/v3d_commands.h`, `hw/v3d_commands.c` | `ClipperXYScaling` takes the vertex shader's Y direction: the clipper's Y scale is negative only for a picture drawn top-down (it always was) | A framebuffer object is drawn bottom-up, and `EXT_clip_control`'s upper-left origin turns either over: the clipper must place the vertices it makes as the shader places the others, as Mesa's `v3dx_emit.c` (`docs/M7_EXTENSIONS_PLAN.md` step 4) |
| M4 | `hw/v3d_context.c` | The render wait's resolution-scaled bound never below `V3D_TIMEOUT_ITERATIONS_RENDER` | A 256x128 target got 8192 polls, a few milliseconds: a frame of 400,000 draws was given up mid-render (A1200 run `ogles2-m4-1`) |
| M4 | `hw/v3d_submit_timeout.c` | The binning and render waits count polls only while the control list executor's current address (`CT0CA`/`CT1CA`) stands still | A fixed poll count timed out a frame of 400,000 draws while the binner was still working, and the next job wedged the GPU (A1200 run `ogles2-m4-1`) |
| M4 | `include/v3d_frame.h`, `hw/v3d_frame.c` | `V3D_MAX_SPILL_BLOCKS` 8 → 96 per binning job, the free pool split off as `V3D_SPILL_POOL` (8), `V3D_RETIRE_MAX` 32 → 128 | A frame of a few hundred thousand draws needs far more than 2 MB of tile lists; the binner then failed and the frame was dropped (A1200 run `ogles2-m4-1`) |
| M4 | `include/v3d_types.h`, `hw/v3d_hw.c` | `v3d_static_buffer` gets an optional `grow` hook; `v3d_buffer_claim_memory` calls it before its overflow fallback | Plan section 9 M4: ogles2 continues an outgrown binning list in a new block (a `BRANCH`) instead of dropping the frame |

Still file-scope state, to move when first used: `hw/v3d_texture.c` (park list, pool),
`hw/v3d_hw.c` (overflow scratch block), `hw/v3d_assembler.c` (assembler work buffers). Process-wide
values that stay global on purpose for now: `g_v3d_cl_generation` (a monotonic cache key, safe to
share), `g_v3d_requested_zbuffer_bits` (read only inside `v3d_context_init`, which ogles2 calls
under its library lock), `g_v3d_render_timeout_iterations` (set by the last context created; a
per-context value comes with the job manager's multi-context work).
