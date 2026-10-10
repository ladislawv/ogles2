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

Still file-scope state, to move when first used: `hw/v3d_texture.c` (park list, pool),
`hw/v3d_hw.c` (overflow scratch block), `hw/v3d_assembler.c` (assembler work buffers). Process-wide
values that stay global on purpose for now: `g_v3d_cl_generation` (a monotonic cache key, safe to
share), `g_v3d_requested_zbuffer_bits` (read only inside `v3d_context_init`, which ogles2 calls
under its library lock), `g_v3d_render_timeout_iterations` (set by the last context created; a
per-context value comes with the job manager's multi-context work).
