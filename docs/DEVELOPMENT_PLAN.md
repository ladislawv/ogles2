# ogles2.library for PiStorm (Raspberry Pi 4, V3D 4.2): development plan

| | |
|---|---|
| Date | Started 2026-10-09; status kept current in §13 |
| Status | In development: M0 to M2 done (real GLES2 programs with offline-compiled shaders), M3 next |
| Baseline | PiStorm3D / minigl.library 29.1 (`MiniGL_Library_68k`, branch `PiStorm3D` @ `01f5a3e`) |
| Target | AmigaOS 3.x, 68k under Emu68 on PiStorm with Raspberry Pi 4B / CM4 (the hardware PiStorm3D supports), RTG (Picasso96 or CyberGraphX) |
| Team | One developer plus an AI assistant (§2.4) |
| Test machine | Amiga 1200 with PiStorm, Raspberry Pi 4B, Emu68 v1.1.0-beta.1; no serial capture (§2.4) |
| Build machine | Ubuntu 24.04 (here in WSL on Windows 10) with both m68k toolchains and a host Mesa build (§2.4, `dev/host-setup`) |
| Priority | GLSL first: runtime shader compilation on the Amiga is M3 |

---

## 1. Summary

The goal is a new shared library, `ogles2.library`, that implements OpenGL ES 2.0 on the Pi 4's V3D 4.2 GPU for 68k AmigaOS. It is built on the PiStorm3D code base:

- **The hardware layer is reused almost unchanged.** PiStorm3D's V3D backend already does device bring-up through the mailbox and device tree, GPU memory, control lists, job submission with binner out-of-memory handling, and texture tiling and mip chains. It becomes ogles2's own "V3D core" library; sharing it with minigl.library comes later (M6). Its front end's RTG display code (screen, window and bitmap targets) is reused nearly verbatim.
- **The front end is new, with PiStorm3D's front end as the template.** The context and display code (`context.c`), the draw path (`draw.c`), textures (`texture.c`) and queries (`others.c`) show how each GLES2 subsystem talks to the hardware. The fixed-function parts (95 hand-written shader variants, matrices, lighting, fog, CPU clipping, display lists, GLU/GLUT) are not carried over.
- **The public API is source-compatible with AmigaOS 4's ogles2.library.** It keeps the same `agl*` context functions and `OGLES2_CCT_*` tag names, and uses the standard Khronos `GLES2/gl2.h`. It is packaged the way minigl.library 29 is: one library vector returns an append-only dispatch table, and clients link a small import library. Code already written against the OS4 ogles2 API (the SDL2/SDL3 OS4 backends, gl4es's `agl` glue, OS4 game ports) can then be ported at source level.
- **Shaders are compiled by Mesa's own V3D compiler,** the code PiStorm3D's assembler and packet definitions already come from.
  - **Stage S1 (offline):** a host tool runs Mesa on Linux or WSL with the `drm-shim` V3D 4.2 emulation, so no Pi is needed, and writes program binaries. The runtime loads them through `glProgramBinaryOES`/`glShaderBinary`, or through a source-hash cache.
  - **Stage S2 (on the Amiga):** the same compiler code is ported to 68k as a separately loaded library, so applications can compile GLSL at runtime.
- **How the driver programs the hardware follows Mesa's gallium `v3d` driver:** its uniform writer, shader-state record, render control list and job/hazard tracking. PiStorm3D already follows Mesa closely for the parts its fixed-function path uses: state packets, the shader-state record and the render list.
- **The work is split into eight milestones, M0 to M7, with GLSL first.** First pixels arrive at M1, real GLES2 programs from offline-compiled shaders at M2, and runtime GLSL compilation on the Amiga at M3. GLES2 core completeness and framebuffer objects follow at M4–M5. The compiler's feasibility spike runs during M0/M1, so the M3 approach is settled early.

---

## 2. Scope

### 2.1 Goals
1. A conformant-in-spirit OpenGL ES 2.0 implementation: all 142 core entry points (Appendix A), GLSL ES 1.00, framebuffer objects, stencil and cube maps.
2. Source compatibility with the AmigaOS 4 ogles2 API (Appendix B), so OS4-oriented code ports with a recompile.
3. Coexistence with minigl.library on the same machine, including both running at the same time (M6). Until then, programs using the two libraries must not run concurrently.
4. Performance that clearly beats the fixed-function MiniGL path for equivalent scenes, by moving all per-vertex work to the GPU and keeping vertex data resident in GPU-visible memory.
5. Foundations for later work: gl4es (desktop GL 1.x/2.x on top of GLES2), SDL2/SDL3 GLES2 renderers, and OpenGL ES 3.x features the hardware supports.

### 2.2 Non-goals for version 1
- OpenGL ES 3.x, compute and transform feedback.
- Raspberry Pi 3 (VideoCore IV / VC4) and Pi 5 (V3D 7.1). The backend's identity check requires V3D 4.2 (`IDENT0 == 0x04443356`).
- Binary compatibility with OS4 executables. OS4 is PowerPC, so only source compatibility is meaningful.
- Sharing objects between contexts (`OGLES2_CCT_SHARE_WITH`) before M6.

### 2.3 Constraints inherited from the platform and from PiStorm3D
- **CPU and toolchain:** bebbo's m68k-amigaos-gcc (now hosted on Codeberg), flags `-mcpu=68020 -m68881 -mcrt=clib2`. **`-fno-strict-aliasing` is required.** PiStorm3D's Makefile documents that strict aliasing deletes its "swivel" byte-swap writes.
  - **GCC 6.5.0b** (the default `amiga6` branch) with clib2 is what PiStorm3D was built with. **Verified on 2026-10-09:** the 29.1 driver sources compiled here appear byte-for-byte in the shipped 29.1 `minigl.library` (27 of 29 objects, including all of the V3D backend; of the other two, `aclip.o` is not linked in and `others.o` differs only in `GLGetIntegerv`), so this is the same compiler. ogles2 uses it for the runtime library.
  - **clib2 needs a local patch.** amiga-gcc fetches adtools clib2 `master` (`c026659`, 2021), which no longer builds against the NDK 3.2 headers the toolchain now ships, and amiga-gcc's `make all` does not build clib2 at all. `dev/host-setup/clib2-ndk32.patch` fixes the three causes (an invalid `##` paste, a `tv_sec` macro that clashes with NDK 3.2's `struct timeval`, and a library base the NDK's varargs wrappers expect). The patched `<time.h>` is installed, so programs that include both `<time.h>` and `<devices/timer.h>` compile.
  - **GCC 16.2.0b** (`amiga16.2` branch, `134541b3686`) is needed for the Mesa compiler port (§8.4), which is C++17. MiniGL Classic V25 was also built with a GCC 16.2.0b build of this toolchain. clib2 does not compile under GCC 16 (its inline asm uses forms GCC 16 rejects), so GCC 16 code uses **libnix** (`-mcrt=nix20`), bebbo's own runtime.
  - **GCC 16.1 (`amiga16.1`) must not be used: it miscompiles.** Functions write callee-saved registers (d2-d7, a2-a6) that their prologue never saves; a ten-line C function reproduces it, and no compiler option avoids it. Found by the stream C spike (§13). A check on GCC's assembly output finds 1,688 such functions in Mesa's compiler sources with GCC 16.1, none with GCC 16.2, and none in PiStorm3D's driver with GCC 6.5.0b (`dev/spike-c/scan_asm.sh`). bebbo's branches keep moving, so that check is re-run after every toolchain update.
  - **GCC 6.5.0b (`amiga6`) drops the truncation of `float`-to-integer conversions** at `-O1` and above (found on the A1200 in M1 run 1, 2026-10-10). Its own RTL pass "bebbo's optimizers", sub-optimisation `e` ("eliminate dead assignments + redundant loads"), deletes the `fintrz.x fp0,fp0` before `fmove.l fp0,dN` as redundant. The conversion then rounds by the FPCR mode, which libnix and clib2 set to round-to-nearest, so `(int)255.5f` is 256: a clear colour channel of 1.0 became 0 in a byte. A real 68881/68882 behaves the same; Emu68 implements the FPCR rounding modes. `double` conversions, `-O0` and GCC 16.2 are correct.
    - **clib2 programs must link `-lm881`, not `-lm`** (found on the A1200 in M2 run 5). With `-mcrt=clib2 -m68881`, `-lm` resolves to libnix's `lib/libm020/libm881/libm.a`, which links without complaint and gives wrong answers in a clib2 program: `sqrtf(2)` is 0 and `cosf` has the wrong sign (esUtil's `esRotate` then rotated nothing). clib2's own 68881 maths library is `clib2/lib/libm020/libm881.a`. The tests and the corpus link it; vbcc builds already did; v3dglslc's m68k build is libnix with soft float, so its `-lm` is the matching one.
    - **ogles2 builds with `-fbbb=abcfhilmnprsz0`**, bebbo's default set without `e`, and every build compiles the library and v3dcore to assembly and fails if `tools/check_fpconv.py` finds a conversion without `fintrz`. Without the flag it finds 11: 5 in ogles2's code, and 5 in PiStorm3D's own `ViewportOffset` and `v3d_texture_emit_state` (so the shipped `minigl.library` 29.1 has them too: worth reporting to its author and to bebbo).
  - The two never mix in one binary: the compiler is a separate module. Because the module uses a different C runtime from the main library, it must take its memory, I/O and logging from the main library through a small call table instead of linking libnix functions that assume a program context (§8.4 spike item).
- **Memory**, as Emu68's source shows (`src/aarch64/start.c`, `src/boards/z2ram.c`, `src/aarch64/vectors.c`, commit `0ba3a89` = tag `v1.1.0-beta.1`, the version the test machine runs):
  - **Pi RAM is identity-mapped.** Emu68 maps all Pi system RAM 1:1, so the 68k address equals the ARM physical address, as cached memory below `0xf2000000` (below 2 GB with the `limit_2g` option). Its Zorro II RAM (`0x200000`–`0x9fffff`, up to 8 MB) is mapped 1:1 too.
  - **The GPU sees the same addresses.** The V3D sits under `/v3dbus`, whose `dma-ranges` is an identity mapping (bus 0 → physical 0, 16 GB), so its bus address equals the physical address. That is why PiStorm3D can hand AllocVec addresses straight to the GPU (`v3d_mem_allocvec.c`). Read from the test machine's device tree on 2026-10-10 (§13).
  - **Chip RAM and Amiga I/O are not GPU-visible.** 68k accesses there fault into Emu68 and go out over the PiStorm bus (`ps_read_*`/`ps_write_*`). The same address seen by the GPU is unrelated Pi memory.
  - **Allocation:** PiStorm3D allocates with `MEMF_PUBLIC | MEMF_REVERSE`, which can fall back to chip RAM when fast RAM runs low. GPU-visible allocations must use `MEMF_FAST` and check the address against the RAM ranges Emu68 publishes in the device tree's memory node (`/memory@0`), which it trims to the usable RAM (§7.3).
  - **Measured on the test machine (2026-10-10):** `/memory@0` holds `0x01000000–0x3C9FFFFF` and `0x40000000–0x79DFFFFF` (about 1.9 GB). All of exec's fast-memory regions (`0x01000000–0x07FFFFFF`, `0x08000000–0x3C9FFFFF`, `0x41000000–0x79DFFFFF`, all priority 40) lie inside them, and so did every test allocation, including PiStorm3D's `MEMF_PUBLIC|MEMF_REVERSE`. Outside them: chip RAM (`0x0–0x1FFFFF`) and a 512 KB region named "memory" at `0x00C00000` (priority −5, `24BITDMA`), which GPU allocations must never use. Emu68 also reserves `0x3D000000` (16 MB, `vc4-mem`) and `0x3CC00000` (2 MB, `unicam-mem`), both outside the ranges.
  - **Cache coherency works through the normal OS calls.** Emu68 translates the 68040/060 `CPUSH` instruction into ARM `dc civac` (clean and invalidate) per line, per page or for the whole cache (`src/M68k_LINEF.c`), so `CachePreDMA`/`CachePostDMA` reach the ARM data cache. A per-line flush loop over a large buffer is slow; the whole-cache form is cheaper for big uploads (measure in M1).
  - **No protection:** the GPU MMU is not used, so a bad GPU address is not caught.
- **Where each part runs. Nothing is installed on the Pi side beyond Emu68's normal SD-card files.**
  - **68k code, translated by Emu68's JIT on ARM CPU0:** the application, `ogles2.library`, its V3D backend (control lists, byte swapping, texture tiling, buffer management) and the S2 compiler module.
  - **The V3D GPU:** binning, rasterisation and the QPU shaders. The backend drives it directly by writing its registers.
  - **The VideoCore firmware** (`start4.elf` on the SD card): the property-mailbox calls the backend makes (firmware revision, `v3d_device.c`); V3D power-up is done by direct power-manager register writes (`power_on_V3D`).
  - **Emu68:** maps the Pi's peripheral windows into the 68k address space as device memory from `0xF2000000` upward (`map_peripheral_ranges` in `src/raspi/start_rpi64.c`), rewrites the device tree's `ranges` to match, and serves `devicetree.resource`. That is why PiStorm3D's V3D registers sit at `0xF2C00000` (`V3D_HUB_BASE` in `v3d_regs.h`) and why it finds the mailbox through the device tree.
  - Moving backend work to a spare ARM core would hit the same obstacle as the compiler (§8.4, no way to run native code there). The heavy work is on the GPU already; the CPU-side costs (byte swapping, tiling) run JIT-compiled and are measured in M1/M6.
- **No GPU interrupts:** completion is detected by polling the binning-flush and render-frame counters. Binner out-of-memory is serviced inside the wait loop (`v3d_submit_timeout.c`).
- **Byte order:** the CPU is big-endian and the GPU little-endian. See §7.4.
- **RTG:** BGRA32 must be the only 32-bit screen mode, `fast_page_zero` must be off, and P96 libraries must all come from one version (PiStorm3D FAQ). Double buffering via `ChangeScreenBuffer` stutters on the Pi 4 RTG stack, which is why PiStorm3D compiles it out (`MGLV3D_DOUBLE_BUFFER_ENABLED 0`).
- **Licences:**
  - PiStorm3D's own and MiniGL-derived code is under the Hyperion MiniGL Open Source License: AmigaOS only, and modifications must be published.
  - The V3D assembler and packet code (Macoy Madson, Broadcom, VMware, Mesa) is MIT-style.
  - Mesa is MIT.
  - See §11 and §12 for the consequences.

### 2.4 Team, machines and development loop
- **People:**
  - **The developer:** decisions, all runs on real hardware, and reporting results and logs.
  - **The AI assistant (Claude):** code, generators, host tools, builds on this PC, analysis of logs and dumps, and plan upkeep.
  - Nobody else is assumed; outside contacts (PiStorm3D maintainers for D3/D6, the OS4 ogles2 author for tag semantics) are needed only at the points named in §12.
- **Build machine (this PC):** Windows 10 with WSL Ubuntu 24.04. Everything runs inside WSL, in its own filesystem (`~`), not `/mnt/c`, for speed.
  - `~/amiga-gcc-install`: bebbo amiga-gcc `6a2b3fb` (2026-10-04), GCC 6.5.0b (`amiga6` at `eb241170d`), with libnix and patched clib2. The PiStorm3D toolchain.
  - `~/amiga-gcc162-install`: the same amiga-gcc commit, GCC 16.2.0b (`amiga16.2` at `134541b3686`), with libnix and libstdc++. For the compiler port.
  - `~/amiga-gcc16-install`: GCC 16.1.1b (`amiga16.1` at `a2807124780`). Miscompiles (§2.3); kept only to reproduce that bug.
  - `~/pistorm3d-release`: the PiStorm3D 29.1 release archive from Aminet (`util/libs/PiStorm3D.lha`, SHA-256 `1b304bf9…0d61`): the shipped `minigl.library`, the demo binaries, the SDK headers, and the `Emu68DeviceTree` tool.
  - `~/mesa-install`: Mesa at the pinned commit `74b6971`, with the gallium `v3d` driver, the v3d drm-shim (stream B) and llvmpipe (reference images, §10).
  - `~/emu68/`: Emu68 `v1.1.0-beta.1` built from source for PiStorm (`dev/host-setup/build_emu68.sh`, Ubuntu's `aarch64-linux-gnu` and `powerpc-linux-gnu` GCC 14 cross-compilers). Not needed for the planned route; it keeps the option of §8.4's ARM worker open, and lets us read the exact firmware the test machine runs.
  - The set-up is reproducible from `dev/host-setup/` next to this plan (scripts, README, package list). Build logs live in `~/toolchain-build/` and `~/mesa-build/`, and the exact commits are recorded in their `summary.log` files.
  - **WSL1 note:** WSL1 has no `memfd_create`, which Mesa's drm-shim needs. A seven-line local patch (`dev/host-setup/drm-shim-wsl1.patch`) falls back to an unlinked temporary file. It touches test tooling only. Moving the distro to WSL2 would make it unnecessary.
  - **Verified on 2026-10-09:** under the drm-shim with `V3D_GPU_ID=42`, Mesa compiles a GLSL ES program into V3D 4.2 QPU code for the fragment, vertex and coordinate (binning) shaders, with the uniform recipes §8.6 describes. llvmpipe renders the same program.
- **Test machine:** the Amiga 1200 with PiStorm on a **Raspberry Pi 4B**, running **Emu68 v1.1.0-beta.1** (variant `pistorm32lite`; source identical to tag commit `0ba3a89`, which §2.3 was read from), **AmigaOS 3.2** (Kickstart 47.115) and Picasso96 with the VideoCore card driver, set up as PiStorm3D requires (§2.3). About 1.9 GB of fast RAM. Details from the baseline run in §13.
- **Loop:** build on this PC → copy to the A1200 → run → forward the log files, screenshots (PPM dumps, §10) and observations → analyse and fix. From the remote test station on (§10.1, D15), the build machine uploads, runs and collects itself over the local network; the developer presses reset after a hard hang and watches where needed.
  - **Copy step:** manual for now; a network share later, which will shorten the loop.
  - **No serial capture.** The developer cannot capture the serial port, so all diagnostics go to files (§10, "Logging without a serial cable").
  - Every test program prints a version stamp, so results can always be matched to a build.
  - Each run should batch several tests, and every test program writes a single result file that can be sent back as is.

---

## 3. Sources gathered

Everything is under `external/`, which `dev/fetch_external.sh` fills at the commits below (it is not part of the repository). The branches of `MiniGL_Library_68k` are separate checkouts.

| Path | What | Commit | Licence | Used for |
|---|---|---|---|---|
| `external/pistorm3d-driver/` | **PiStorm3D driver source**: `backend/` (V3D layer) + `gl/` (MiniGL front end), 29 `.c`/26 `.h`, ~54k lines | `01f5a3e` 2026-10-08 (branch `PiStorm3D`) | Hyperion + MIT parts | Backend reuse; front-end template |
| `external/minigl-classic/libraryroot/` | **minigl.library 29.1 shared-library glue**: ROMTag, single-LVO dispatch, client import lib, freestanding runtime shims, ABI tests | `d950a3a` 2026-10-08 (branch `MiniGL_Classic`) | Hyperion | Library packaging template |
| `external/minigl-library-v3d/` | Older (Sept 2026) copy of the same glue | `7a07368` | Hyperion | History only |
| `external/MiniGL_Library_68k/` | Main clone (release README only; the sources are on its branches) | `b899c47` | n/a | Release notes |
| `external/Warp3DPiStorm4/` | Hardware docs (BCM2711 and BCM2835 peripherals, VideoCore IV reference), V3D link list, early `v3dAssembler.h` | `541fc31` | various | Background |
| `external/reference/mesa/` | Mesa, sparse: `src/broadcom/` (compiler, QPU, CLE packet XML, simulator, drm-shim, common), `src/gallium/drivers/v3d/`, `include/drm-uapi/v3d_drm.h` | `74b6971` 2026-09-25 | MIT | Compiler; driver reference |
| `external/reference/sdl-os4/` | AmigaPorts SDL (SDL2 branch), `src/video/amigaos4/` incl. `SDL_os4opengles2.c` | `fca1fa7` | zlib | Template for SDL integration; real-world `agl*` usage |
| `external/reference/gl4es/` | gl4es, sparse: `src/agl/` (AmigaOS 4 glue), licence | `ec16bed` | MIT | `agl*` usage; future gl4es port |
| `external/reference/glsnoop/` | glSnoop tracer; hooks every ogles2.library function and decodes every `OGLES2_CCT_*` tag | `d0173ba` | Public domain | Authoritative OS4 ogles2 API surface (Appendix B) |
| `external/reference/opengl-registry/` | Khronos `api/GLES2/` headers + `xml/gl.xml` | `6af574a` | Apache-2.0 / MIT | Public headers; code generation |
| `external/reference/egl-registry/` | Khronos `api/KHR/khrplatform.h` | `db3425b` | MIT | Public headers |

Release archive (outside `external/`, in WSL at `~/pistorm3d-release`): **PiStorm3D 29.1 from Aminet** (`util/libs/PiStorm3D.lha`, 2026-10-07). It holds `bin/minigl.library` (`$VER: minigl.library 29.1 (5.10.2026)`, built 2026-10-05), the demo binaries `cube`, `gears`, `library_test`, `ring` and `warp` with their data, the SDK (headers and the client import library `libminigl.a`), the licences, and the `Emu68DeviceTree` tool.

Not present anywhere:
- the sources of the demo and test programs mentioned in the README (only the binaries are in the release);
- the extra objects the shipped `minigl.library` was linked with. The published glue cannot link the 29.1 driver on its own: `realloc`, `strtol`, `exit` and `atexit` (called by `dlist.c`, `draw.c` and `glut.c`) are undefined, yet those objects are in the shipped binary. They were evidently supplied through the Makefile's `PISTORM3D_EXTRA` hook. ogles2 writes its own versions (§4.2).

The repository `SteffenHaeuser/PiStorm3D` is empty, and the sources live on the branches above.

---

## 4. What PiStorm3D provides

### 4.1 How a MiniGL draw reaches the screen today
1. `glBegin/glEnd` or vertex arrays fill `MGLVertex` buffers. The CPU gathers and byte-swaps vertex data into `state_buf` on every draw (`draw.c`, `d_Pack*`).
2. `gl_EmitPrimitiveV3DEx` (`draw.c`, ~2,400 lines) does the following:
   - selects one of 95 pre-assembled shaders;
   - writes the vertex, coordinate and fragment uniform streams (matrix, viewport scale, depth range, lighting tail, TMU config pairs);
   - builds the GL shader-state record and attribute records (`d_BuildShaderRecord`);
   - emits deduplicated state packets (`gl_EmitCullBlendState`);
   - appends the primitive packet to the binning control list.
3. `gl_FrameBegin` (`context.c`) starts the binning list for a pass. `gl_FramePresent` builds the render list, locks the RTG bitmap (`LockBitMapTags`) and stores the tile buffer to it as raster BGRA. It submits binning and **waits for binning to finish**, servicing binner out-of-memory in that wait. It then submits render and returns without waiting. The render wait happens later, in `MGLFlushPendingRender`.
4. `MGLSwitchDisplay` presents the frame. Fullscreen draws directly into the screen bitmap. Windowed mode uses `ClipBlit` from an offscreen bitmap. A host-owned bitmap has nothing to present.

### 4.2 Reuse map

| PiStorm3D file | What it does | Decision |
|---|---|---|
| `backend/hw/v3d_device.c`, `v3d_regs.h`, `devicetree_protos.h`, `lvocall_compat.h` | Mailbox via `devicetree.resource`, firmware check, V3D power-on, identity check, performance counters | **Reuse** (move to V3D core). One addition: `v3d_read_info` fills only `ver` and `has_accumulators`. It must also fill `vpm_size` and `qpu_count`, which the struct already has but nothing sets; read them from the identity registers as Mesa's `v3d_device_info.c` does. VPM configuration (§7.9) and spill sizing (§8.6) need them, and the viewport uniforms need Mesa's `clipper_xy_granularity` (256 on V3D 4.2, a new field) |
| `backend/hw/v3d_mem_allocvec.c` | AllocVec buffers, 64-byte aligned, bus address = CPU address | **Reuse**, wrapped in a buffer object (§7.3); switch to `MEMF_FAST` with an address-range check (§2.3) |
| `backend/hw/v3d_hw.c` / `v3d_hw.h` | Register access, L2T/slice cache invalidation, control-list buffer primitives, CPU tiling (UIF/UBLINEAR/LINEARTILE), **every V3D 4.2 packet and record struct** (incl. `STENCIL_CFG`, `BRANCH`, TMU parameter 2, all texture data formats incl. ETC2/BC/ASTC, depth/stencil formats) | **Reuse**. New packets should use generated byte-wise packers (§4.3) |
| `backend/hw/v3d_commands.c` | Packet emitters (`TileBinningModeCfg`, `CfgBits`, `BlendCfg`, `StoreTileBufferGeneral`, `glShaderState`, `IndexedPrimList`, …) | **Reuse**; add stencil, branch and missing emitters |
| `backend/hw/v3d_clbuf.c` | Growable state buffer with deferred free of the old block | **Reuse**; extend the binning list with `BRANCH` chaining |
| `backend/hw/v3d_frame.c` | Pool sizing, binner spill pool, retire list keyed by job sequence | **Reuse**; turn file-static state into device state |
| `backend/hw/v3d_submit_timeout.c` | Submit binning/render, polling waits with OOM service and timeouts | **Reuse**; add sequence numbers and the arbitration lock (§7.1) |
| `backend/hw/v3d_context.c` | Per-target pools (Z, tile state, tile alloc), two control-list slots, resize, scratch colour, DMA flush | **Adapt**: split into per-context control-list slots and per-render-target pools |
| `backend/hw/v3d_texture.c` | Layout decisions (Mesa's `v3d_setup_slices`), lazy mip chain, upload/sub-image, rename/park for mid-frame reuse, texture and sampler state emission. Assumes 4 bytes per texel throughout (`*_rgba8` entry points, strides of `level_w * 4`) | **Adapt**: element sizes 1/2/4/8, formats, swizzles, cube maps, render-target use |
| `backend/hw/v3d_assembler.c` / `.h` | Macoy Madson's port of Mesa's QPU code (packer, disassembler, validator) plus a text assembler (ALU only, no branches) and 95 fixed-function shaders | **Keep** packer, disassembler and validator; keep the text assembler for a few internal shaders (clear quad, blit); **drop** the 95 variants |
| `gl/src/context.c` | Display: `vid_OpenDisplay` / `vid_OpenWindow` / `vid_AdoptWindow` / `vid_AdoptBitMap`, bitmap locking, `gl_FrameBegin`/`gl_FramePresent`, clears (`GLClear`, `gl_ClearViaQuad`), readback, `MGLSwitchDisplay`, `MGLFlushPendingRender` | **Template**: display code moves nearly verbatim; frame code generalises to jobs (§7.7) |
| `gl/src/draw.c` | Draw-state residue, state-packet dedup cache, shader-record and attribute-record building, texture-state cache, early-Z accumulation, index buffers | **Template** for the GLES2 draw path (§7.9); variant selection dropped |
| `gl/src/texture.c` | `glTexImage2D`/`SubImage`/`CopyTex*`, unpack rules, `tex_SyncBeforeModify` | **Template** |
| `gl/src/others.c`, `viewport.c`, `init.c` | `glGet*`, `glReadPixels`, strings, viewport math, library opening | **Template** |
| `gl/src/vertexarray.c`, `vertexelements.c` | Client-array gather | **Template** only for client-side arrays |
| `gl/src/matrix.c`, `light.c`, `fog.c`, `hclip.c`, `aclip.c`, `dlist.c`, `glu.c`, `glut*.c`, `vertexbuffer_min.c` | Fixed function, immediate mode, CPU clipping, utility libraries | **Not used** |
| `libraryroot/library/minigl_library.c` | ROMTag, `LibInit/Open/Close/Expunge`, single vector at −30 | **Model only**: same structure, written fresh (D14) |
| `libraryroot/library/dispatch_table_pistorm3d.c` | Hand-written positional dispatch table | **Replaced** by a generated table (§6.6) |
| `libraryroot/library/runtime_shim.c`, `math_shim.c`, `memcmp_shim.c`, `pistorm3d_stubs.c`, `*.s` | Freestanding libc for a resident library | **Reuse**; add what is missing (`realloc` at least, §3). A link test that fails on any undefined symbol catches the rest |
| `libraryroot/client/*` | `MiniGLOpen/Close`, base pointer, vector stub | **Template** |
| `libraryroot/Makefile_dispatch.gcc`, `pistorm3d-driver/Makefile` | Build | **Template** |
| `libraryroot/tests/test_dispatch_abi.c`, `verify_built_dispatch.py` | ABI checks | **Template**; generate |

### 4.3 Lessons from PiStorm3D to apply from day one
1. **Hand-packed hardware structs are fragile.** Records are C bitfields declared from the most significant bit down, so that the big-endian compiler's layout matches the hardware once each 32-bit word is byte-swapped by hand ("swivel": `swivel[3] = LE32(swivel[3])` in `d_BuildShaderRecord`). Each new struct is a new chance to swap the wrong word.
   - *Do:* generate packers for new packets from Mesa's `src/broadcom/cle/v3d_packet.xml` with `gen_pack_header.py`, **with one patch**. Mesa's packers assign most fields byte by byte (`cl[n] = …`), which is independent of byte order. But they copy whole 32-bit `float`/`uint`/`int` fields with `memcpy` in host order. On the big-endian 68k those would come out byte-reversed, so the generator must emit little-endian stores for them. Existing hand-packed structs can migrate gradually.
2. **Parallel tables maintained by hand drift.** Examples: the variant enum, upload order and selectors; three dispatch tables that must grow together; stale comments citing 114 variants and 143 KB.
   - *Do:* generate the dispatch table, client wrappers, `aglGetProcAddress` table and ABI test from one source (§6.6).
3. **Frames are dropped instead of handled.** Binning, render and tile-list buffers are fixed-size, and an overflow drops the frame. If `state_buf` grows partway through a draw's uniform writes, the frame is marked corrupted and dropped.
   - *Do:* size each uniform stream from its recipe and claim it in one piece; chain the binning list with `BRANCH` packets (as Mesa's `v3d_cl_ensure_space_with_branch` does).
   - The render and tile lists need no chaining. Their size is bounded and known up front: Mesa reserves 200 bytes plus room for 256 `SUPERTILE_COORDINATES` packets, the supertile maximum (`v3dx_rcl.c`).
4. **State that should belong to the device or context lives in file statics:** the retire list, texture park list, dedup cache, mailbox and frame counters. That assumes one context per machine.
   - *Do:* put it in device or context objects from the start.
5. **Presentation needs its own work.** Single buffering is the shipped default because `ChangeScreenBuffer` stutters. Plan an explicit investigation (M6) instead of inheriting the switch.
6. **ABI rules from minigl:** append-only dispatch table, `abiVersion` compared for equality, `structSize` compared as "at least mine". Keep all three from day one.

---

## 5. Target architecture

```
 Application (GLES2 + agl*)                 SDL2/SDL3 (ogles2 context), gl4es
        |  <proto/ogles2.h>  inline wrappers -> OGLES2Dispatch->fn(ctx, ...)
        v
 libogles2.a (client import lib, per program image)
   OGLES2Open/Close, current-context variable, ctx-less thunks for aglGetProcAddress
        |  one library vector (-30) returns the dispatch table
        v
 ogles2.library  (resident, 68k)
   +-- API layer (generated table, argument validation, GL errors)
   +-- Object model: contexts, buffers, textures, shaders, programs, FBOs, renderbuffers
   +-- State tracker: GLES2 state, dirty bits, packet dedup
   +-- Program runtime: V3DP loader, uniform recipe writer, shader-state builder
   +-- Job manager: per-framebuffer jobs, hazards, flush/finish, presentation
   +-- Display: fullscreen screen / window+offscreen bitmap / host bitmap
        |                                    |
        v                                    v (lazy, on cache miss, S2 only)
 V3D core (shared with minigl from M6)      v3dglsl compiler module (Mesa GLSL+NIR+broadcom, 68k)
   device, memory, CL buffers, packets,
   submission + arbitration, tiling, textures
        |
        v
 V3D 4.2 GPU (Pi 4) via Emu68 / PiStorm

 Host side (Linux/WSL): v3dglslc = the S2 code built natively (Mesa GL + V3D driver on the fake DRM device) -> .v3dp program binaries
```

Components:
- **`ogles2.library`**: the resident library with all API and driver logic.
- **`libogles2.a`**: the import library. It holds `OGLES2Open()`/`OGLES2Close()`, the client-side current context, and the `aglGetProcAddress` thunk table.
- **Headers:** `<proto/ogles2.h>` (umbrella), `<libraries/ogles2.h>` (tags, error codes, dispatch struct), and verbatim from Khronos: `<GLES2/gl2.h>`, `<GLES2/gl2ext.h>`, `<GLES2/gl2platform.h>` (which `gl2.h` includes) and `<KHR/khrplatform.h>`. These need no Amiga changes: `KHRONOS_APICALL` is empty on this platform, and the integer types come from `<stdint.h>`.
- **V3D core** (`libv3dcore.a`): a copy of PiStorm3D's `backend/`, with its globals moved into a device object. It is ogles2's own copy until minigl.library adopts it (M6, D3), and possibly a shared component after that.
- **`v3dglslc`**: the host tool for stage S1.
- **The 68k compiler module**: stage S2.

---

## 6. Public API and ABI

### 6.1 Library identity
- **Name:** `ogles2.library`, version 1. This is the same name as on OS4, which helps porting and documentation. OS4 is a different OS, so the names don't collide on disk. *Decision D1 in §12.*
- **Pluggable backends:** `backendFlags` in the table (as in minigl) leaves room for a future non-PiStorm implementation, for example a stub or a Pi 3/VC4 backend, behind the same ABI.

### 6.2 Dispatch table and the context argument
- The library has one public vector at −30, `OGLES2GetDispatchTable()`. This is the mechanism minigl.library uses: `LibGetDispatchTable` on the library side, reached through the client stub `MiniGLGetDispatchTableLVO`.
- The table header is `abiVersion, structSize, backendFlags, reserved`, followed by function pointers.
- **The `ctx` argument comes first** in every `gl*` entry, and in every `agl*` entry that acts on the current context (`aglSwapBuffers`, `aglSetParams2`, `aglSetBitmap`): `struct OGLES2Context *ctx`. This follows PiStorm3D's internal `GLFoo(GLcontext context, …)` convention.
  - `aglCreateContext2`, `aglDestroyContext` and `aglMakeCurrent` take only their own arguments (§6.4).
  - `aglGetProcAddress` never reaches the library (§6.5).
- The client wrapper passes the **current context of this program image**, kept in a variable in `libogles2.a`. `<proto/ogles2.h>` follows the pattern of minigl's `minigl_dispatch.h`:
  - it includes `<GLES2/gl2.h>` itself, with `GL_GLES_PROTOTYPES` set to 0;
  - it defines each wrapper as a `static __inline__` function under an `OGLES2_` prefix;
  - it maps the GL name onto that function with a macro.

  A wrapper defined directly under the GL name would clash with the `extern` prototypes `gl2.h` declares. The macro form works in whichever order the application includes the headers.

```c
/* <proto/ogles2.h>, generated */
static __inline__ void OGLES2_glViewport(GLint x_, GLint y_, GLsizei width_, GLsizei height_)
{ OGLES2Dispatch->GLViewport(OGLES2CurrentContext, x_, y_, width_, height_); }
#define glViewport OGLES2_glViewport
```

- **Field names are `GLViewport`, as in minigl's table** (found while building M0). `glViewport` would be rewritten by the wrapper macro itself, and the bare `Viewport` form collides with AmigaOS macros: `<proto/exec.h>` defines `Enable()` and `Disable()`. Wrapper parameters carry a trailing `_`, so an application macro named like a `gl.xml` parameter cannot break the header.

- **Import-library wrappers keep the current context in step with the library:**
  - `aglMakeCurrent(ctx)` sets `OGLES2CurrentContext` and calls the library, so the library can bind the display state;
  - `aglDestroyContext(ctx)` clears `OGLES2CurrentContext` if it names the destroyed context.
- **Why this design:**
  - The current context is per process by construction, because each program links its own import library. Two applications can use the library at once, which minigl's single global context cannot do.
  - Calls cost no lookup.
  - Function pointers map one-to-one onto internal functions.
- **Limitations (document them):**
  - Every caller inside one program image must share one import-library copy. A wrapper library that itself links `libogles2.a` must forward the `agl*` calls rather than keep its own current context.
  - The current context is per program. GLES/EGL specify one per thread, which here would mean one per task. A program that issues GL calls from several tasks must serialise them itself and must not use different contexts concurrently.

### 6.3 Calling convention rules
- **Arguments are passed on the stack** (the GCC m68k default). The vbcc client path needs a test: minigl ships vbcc makefiles and a `test_dispatch_abi.c`, which is the model.
- **No table function returns `float` or `double`.** GLES2 has none, since `glGet*` use output pointers, so register-return differences between compilers (`d0` versus `fp0`) never arise. Keep it that way for extensions.
- **`GLboolean` is 1 byte (`unsigned char`) in Khronos headers,** whereas MiniGL used `unsigned int`. Use the Khronos types; the ABI test checks argument layout.
- **Verified for GCC 6.5.0b and vbcc 0.9h (2026-10-10, `tests/abi_probe`, 30 checks from each compiler against the same GCC-built library, in the emulator):**
  - byte arguments: both push a 4-byte slot with the value in its last byte, which the library reads (`move.b 15(a5)`);
  - float arguments: 4-byte single precision on the stack, alone, after an integer and before a `GLboolean`;
  - `GLboolean` return values: the GCC-built library sets only the low byte of `d0` (`move.b d2,d0`), and both compilers' callers zero-extend it themselves (`and.l #255,d0` in vbcc), so the upper bytes never matter;
  - varargs tag lists and `aglGetProcAddress` thunks behave the same.
- **vbcc clients** link `ogles2.lib`, the import library built with vbcc (`make vbcc`: the same C sources plus a vasm copy of the vector stubs). The generated `<proto/ogles2.h>` uses `static inline` for vbcc in C99 mode (`-c99`) and plain `static` otherwise. vbcc compiles against the NDK 3.2 headers of the GCC toolchain unchanged.

### 6.4 Context API (OS4-compatible names)

| Function | Plan |
|---|---|
| `aglCreateContext2(ULONG *errcode, struct TagItem *tags)` / `aglCreateContextTags2(&err, tag, …)` | Main constructor; tags in Appendix B |
| `aglCreateContext(…)`, `aglSetParams(…)` | Deprecated on OS4: its interface names these slots `aglCreateContext_AVOID` and `aglSetParams_AVOID`. Provide them only as macros onto the "2" versions, so old source still compiles |
| `aglDestroyContext(ctx)` | Frees everything; leaves an adopted window or bitmap alone (PiStorm3D's `ExternalWindow`/`ExternalBitMap` polarity rules) |
| `aglMakeCurrent(ctx)` | Sets the client-side current context and tells the library |
| `aglSwapBuffers()` | Flushes jobs for the default framebuffer, then presents (§7.2) |
| `aglSetParams2(tags)` / `aglSetParamsTags2(…)` | Change window or bitmap, vsync, resize (uses `v3d_context_resize`) |
| `aglSetBitmap(bitmap)` | Retarget to a host bitmap (`vid_AdoptBitMap` rules: 4 bytes per pixel, CGX-lockable) |
| `aglGetProcAddress(name)` | Returns context-free thunks from `libogles2.a` (§6.5) |

Tag *names* follow OS4 (taken from glSnoop). Their *values* are our own, because only source compatibility is in scope and the OS4 SDK headers are distributed with the commercial Enhancer pack and must not be copied.

### 6.5 `aglGetProcAddress`
- SDL's GLES2 renderer (through `SDL_GL_GetProcAddress`, which the OS4 backend maps to `aglGetProcAddress`) and gl4es (through its own name table) look GL functions up by name and call them **without** a context argument.
- `libogles2.a` therefore contains generated thunks with the exact GL signatures and a sorted name table resolved with a binary search. This needs no library call. For example: `static void thunk_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { OGLES2Dispatch->GLViewport(OGLES2CurrentContext, x, y, w, h); }`.
- Unknown names return NULL.

### 6.6 Code generation
A host-side Python script reads Khronos `xml/gl.xml` for the GLES 2.0 feature set and the chosen extensions, plus a small hand-written list of `agl*` and Amiga-specific entries. It generates:
- the `struct OGLES2Dispatch` declaration (append-only order, recorded in a checked-in `abi_order.txt` that the generator refuses to reorder);
- the library-side table initialiser, so it can never be shorter than the struct (minigl's "ALL THREE TABLES GROW TOGETHER" hazard);
- the client inline wrappers, thunks and name table;
- stubs that record `GL_INVALID_OPERATION` for entries not yet implemented during development;
- the ABI test program and a post-build checker modelled on `verify_built_dispatch.py`.

### 6.7 Versioning rules
- **`OGLES2_DISPATCH_ABI_VERSION` = 1**, compared for **equality** in `OGLES2Open()`. Bump it only for a deliberate break.
- **`structSize >= sizeof(OGLES2Dispatch)`.** Appended entries need a new library version, never an ABI bump.
- **`OGLES2_VERSION`** is the minimum library version passed to `OpenLibrary`.

---

## 7. Subsystem design

### 7.1 V3D core and GPU arbitration
- **Copy `backend/` into `libv3dcore.a`** with a `V3DDevice` that owns what is now file-static: the mailbox, retire list, texture park list, timeout scaling and statistics counters.
- **Any two GPU clients can collide:** two ogles2 programs, or ogles2 and minigl.library. They program the same control-list queue registers and wait on the same frame counters. `v3d_wait_render_timeout` treats *any* counter increment as "my job finished", so interleaved jobs from two clients break each other.
  - **Phase 1 (M0 for ogles2; minigl.library joins in M6):** a **public semaphore** (`AddSemaphore`/`FindSemaphore`, for example `"V3D 4.2 submit"`) held from submit until the wait completes.
    - Whichever library starts first creates it inside `Forbid()`/`Permit()`, after a `FindSemaphore` shows it does not exist yet.
    - It is allocated in public memory and never removed, because either library may still reference it.
    - It is simple and correct, but serialises the GPU.
    - PiStorm3D defers the render wait to the start of the next frame (`MGLFlushPendingRender`). Held across that, the lock would cover a whole frame of CPU work. In Phase 1 both libraries therefore wait for their own render before releasing. This gives up the small CPU/GPU overlap PiStorm3D has today, which Phase 2 restores.
    - Acquire with `AttemptSemaphore` in a loop with a short wait and an overall timeout, so a crashed holder cannot hang the other library forever. Use `timer.device` for the wait rather than `Delay`, because `Delay` requires the caller to be a DOS process.
  - **Phase 2 (M6):** a shared submission component (a resource or library) that hands out sequence numbers and tracks completion in order. This allows CPU/GPU overlap across clients.
  - **The minigl.library side of both phases is deferred to M6** and needs the maintainers' agreement (D3).
    - Until then, ogles2 and minigl.library programs must not run at the same time; the documentation says so.
    - ogles2 warns on the serial log at context creation if `minigl.library` is open (found in `SysBase->LibList` with a non-zero open count).
- **Keep polling.** Add a job sequence number to every submission and a debug "GPU hang report" (PiStorm3D's `CT1CA` wedged/advancing diagnostic is already good).

### 7.2 Contexts, display and presentation
- **Context creation** reuses PiStorm3D's three display paths (`context.c`):
  - **fullscreen** (`OGLES2_CCT_MODEID`, or the best BGRA32 mode): `vid_OpenDisplay`, with a borderless backdrop window for input;
  - **window** (`OGLES2_CCT_WINDOW`): `vid_AdoptWindow`, an offscreen 32-bit bitmap plus `ClipBlit` on swap;
  - **host bitmap** (`OGLES2_CCT_BITMAP` / `aglSetBitmap`): `vid_AdoptBitMap`, where the host presents.
- **Default framebuffer:**
  - colour is the RTG bitmap, stored from the tile buffer as raster BGRA with the R/B swap PiStorm3D already uses, rendered Y-flipped as described in §7.11;
  - depth/stencil is UIF-tiled in GPU memory. The tile buffer's internal depth type is one of `depth_32f`, `depth_24` or `depth_16`. Stencil works alongside any of them: Mesa stores it separately (`S8`) when the depth format is D32F.
    - When `OGLES2_CCT_STENCIL` > 0: `depth_24`, stored to memory as **D24S8** (`V3D_OUTPUT_IMAGE_FORMAT_D24S8`).
    - Otherwise `OGLES2_CCT_DEPTH` decides: 16 gives D16; any other non-zero value gives D32F (PiStorm3D's `zbuffer_bits` logic). No stencil is stored.
    - When both are 0 (SDL passes its own `depth_size`/`stencil_size`, which a 2D program may set to 0), no depth/stencil memory is allocated. As GLES2 specifies for a framebuffer without those buffers, the depth and stencil tests then behave as if disabled whatever their enables say.
  - **Channel sizes:** the colour buffer has 8 bits each of red, green, blue and alpha (the BGRA32 alpha byte is stored, though the display ignores it). `GL_DEPTH_BITS` and `GL_STENCIL_BITS` report the format chosen above. `GL_DST_ALPHA` blending therefore reads the stored alpha, as on any RGBA8 surface.
  - **Depth and stencil are not stored at swap, and not loaded at the start of the next frame.** EGL leaves ancillary buffers undefined after a buffer swap, and GLES2 programs normally clear them every frame. So the final pass of a frame never stores depth/stencil, and the first pass clears rather than loads them. Only a mid-frame pass split stores them, because the next pass of the same frame loads them.
    - This replaces PiStorm3D's `depth_persist_seen` heuristic, which can cost one corrupted frame.
    - A PiStorm-specific tag (D10) can opt back into preserving them.
- **`aglSwapBuffers`:** flush pending jobs for the default framebuffer (§7.7), then present.
  - **v1 behaviour:** single-buffered fullscreen, or `ClipBlit` for windows, as MiniGL does.
    - With the Phase 1 arbitration lock (§7.1), every flush waits for its own render before releasing the lock, so the swap returns with the frame complete.
    - MiniGL's deferred wait (`MGLFlushPendingRender` at the start of the next frame) comes back only with Phase 2.
  - **M6:** double or triple buffering and real CPU/GPU overlap. The two control-list slots already exist; the blocker is the wait in `gl_FrameBegin` plus a single render target. Investigate the `ChangeScreenBuffer` stutter, including alternatives such as `WaitTOF` with a blit from a back buffer.
- **Vsync** (`OGLES2_CCT_VSYNC`): `WaitTOF` before presenting, or `ChangeScreenBuffer`'s safe message once double buffering works.
- **Resize** (`OGLES2_CCT_RESIZE_VIEWPORT`, `aglSetParams2` with a new window): `v3d_context_resize`, then reset the viewport if requested.

### 7.3 Memory: buffer objects, coherency, deferred release
- **`V3DBO`** = `v3d_mem` + size + flags + `last_job_seq` + CPU-dirty range.
- **All GPU-visible allocations go through it:** vertex/index copies, textures, render buffers, shader code, uniform storage copies and spill memory.
  - Allocate with `MEMF_FAST`.
  - Reject, and report `GL_OUT_OF_MEMORY` for, any block outside the RAM the GPU can reach (§2.3). That RAM is the ranges of the device tree's memory node (`/memory@0`; look the node up by `device_type = "memory"` rather than by name), read once at device init through `devicetree.resource`, which `v3d_device.c` already opens. Zorro II RAM (`0x200000–0x9FFFFF`, when enabled) is Pi RAM as well, but lies below the published ranges (which start at `0x01000000` on the test machine), so the check rejects it. That is safe, merely conservative. If a rejected block is ever a problem in practice, allocate one large pool from a published range at device init and sub-allocate from it.
- **Coherency rule, enforced at submit:** every BO referenced by a job with a CPU-dirty range gets `CachePreDMA` on that range. This replaces the scattered flushes in PiStorm3D (`gl_EnsureShaders`, texture upload, `v3d_context_flush_for_dma`) with one mechanism. Readback paths call `CachePostDMA` after the wait.
- **Freeing while in use:** BOs referenced by unfinished jobs go to the retire list (`v3d_frame_defer_free`, generalised) and are released after a *successful* wait for that job.
- **Renaming:** modifying a buffer or texture that a pending job references allocates a fresh BO and parks the old one. This generalises `v3d_texture_rename` / `tex_SyncBeforeModify`; the fallback when out of memory is flush and wait.
- **Measured on the test A1200 (M1):** the device tree gives `0x01000000-0x3C9FFFFF` and `0x40000000-0x79DFFFFF`; exec's three large fast regions lie inside, while chip RAM and a 512 KB fast region at `0x00C00000-0x00C7FFFF` lie outside. The GPU did read vertex data from that region (and from the top of RAM), so the range check is conservative there, as intended. `v3d_mem_alloc` now asks for `MEMF_FAST` (it asked only for `MEMF_PUBLIC`, which could have returned chip RAM) and gives back any block outside the ranges; the library logs every region at device init.
- **Accounting:** keep the device's `BytesAllocated`, which PiStorm3D's `v3d_free` already checks for leaks at device shutdown. Report current/peak usage through a debug call. Track usage per context as well, so a leak can be attributed while other contexts are still alive.
- **Cleanup when a program exits without `aglDestroyContext`:** the library records each context's owning task. `LibClose` runs in the task of the program closing the library (normally through `OGLES2Close`). It waits for that program's outstanding render, releases the arbitration lock if that program holds it, and destroys every context the task still owns.
  - Expunging the library must never leave GPU memory or a locked RTG bitmap behind.
  - A program that crashes without closing the library cannot be cleaned up this way. Its contexts stay allocated until reboot, as with any Amiga resource leak, but the lock timeout (§7.1) keeps other GPU users working.

### 7.4 Byte-order rules

| Data | Written by | Rule |
|---|---|---|
| Control-list packets, shader-state and attribute records, texture/sampler state | Driver | Little-endian via existing helpers or generated byte-wise packers |
| Shader code | Compiler | Stored little-endian in the `.v3dp` container and copied without a swap (PiStorm3D swaps with `byteswap64` at upload today) |
| Uniform streams | Driver, per draw | Every 32-bit word LE32 (`swap_float32_into`) |
| VBO contents | Application, big-endian, typeless | GPU copy byte-swapped per attribute element size on first use with a given layout (§7.5); byte attributes untouched |
| Client-side vertex arrays | Application | Copied into `state_buf` with swaps, per draw |
| Index data | Application | 16/32-bit swap; GPU copy cached per index type |
| Textures: RGBA8, RGB8, L8, A8, LA8, ETC1 | Application | Byte streams, converted or tiled with no swap |
| Textures: 565/4444/5551 (16-bit), float (32-bit) | Application | Swapped during upload, inside the tiling loop |
| RTG colour buffer | GPU | Tile store with R/B swap into BGRA32 (as today) |
| `glReadPixels` RGBA/UNSIGNED_BYTE | GPU → app | From the BGRA screen: byte copy plus BGRA→RGBA reorder, rows taken bottom-up (GL's origin is the bottom-left, §7.11). From an FBO texture: de-tile, no reorder, rows in memory order |
| Registers | CPU ↔ GPU | `LE32` (`v3d_regs.h`; note its two pre-swapped/natural conventions) |
| Uniform recipe data in the `.v3dp` (e.g. TMU config words) | Compiler | Host-order numbers inside the compiler, LE32 in the container. A packer's output (GPU bytes) must be converted with `util_le32_to_cpu` before it is used as a number; Mesa's V3D compiler did not, which the stream C spike caught (`patches/0008`) |

### 7.5 Buffer objects and vertex input
- **V3D fetches GLES2's attribute types natively.** `GL_FLOAT`, `GL_BYTE`/`GL_UNSIGNED_BYTE` and `GL_SHORT`/`GL_UNSIGNED_SHORT` (normalised or not) map to `v3d_ATTRIBUTE_FLOAT/BYTE/SHORT` with the signed and normalised flags, so the CPU only swaps bytes.
  - **`GL_FIXED`:** the attribute record has a "fixed" type (`v3d_ATTRIBUTE_FIXED`), which Mesa never uses (its state tracker converts fixed to float). **The M1 probe showed it reads GL's 16.16 exactly** (runs 8 to 10, the same picture as the float path), so `GL_FIXED` arrays go to the GPU byte-swapped only, like floats.
  - **`v3d_ATTRIBUTE_HALF_FLOAT`** serves `OES_vertex_half_float` if that extension is added.
- **`glBufferData`/`glBufferSubData`** keep a **CPU shadow** in application byte order. GPU copies are built lazily, keyed by a **layout signature**: the list of (offset within stride, element size, count) over all attributes that source the buffer, or the index type for element arrays. Common cases are cheap:
  - all-float attributes (and all-fixed, if the hardware's fixed type works): one 32-bit swap of the whole buffer;
  - element arrays: one 16-bit or 32-bit swap;
  - mixed interleaved layouts (float position + ubyte colour): per-element swap following the signature, cached.
- **Dirty ranges** from `glBufferSubData` re-swap only the affected range in every cached copy. If a pending job references the copy, it is renamed instead (§7.3).
- **Memory:** for `GL_STATIC_DRAW` buffers whose layout never changed after N draws, the shadow may be dropped. PiStorm systems have plenty of RAM, so this is optional.
- **v1 as built (`buffer.c`, M2):** one GPU copy per attribute layout (offset, size, type, stride, normalised), of the whole buffer from that offset, packed at a 4-byte stride as client arrays are; up to 8 layouts per buffer, kept until the data change. `glBufferData` and `glBufferSubData` drop every copy (the changed range is not re-swapped alone yet); a copy the open job reads is retired with that job (`v3d_frame_defer_free`), so a change between two draws of one frame neither waits nor flushes. Element buffers are read from the shadow and copied per draw, as client indices are. A draw that would read past a buffer's end is not drawn (logged).
- **`OES_mapbuffer`:** `glMapBufferOES` returns the shadow and `glUnmapBufferOES` marks it fully dirty.
- **Client-side arrays** (legal in GLES2):
  - `glDrawArrays`: copy and swap `[first, first+count)` per attribute into `state_buf`;
  - `glDrawElements` with client indices: scan the indices for min/max first and copy only that range, as Mesa's state tracker does. PiStorm3D instead gathers vertex by vertex through the index list, which is not suitable for VBO-style attribute records.
- **Disabled arrays** (`glVertexAttrib4f` constants): an attribute record with stride 0 pointing at a 16-byte constant in `state_buf`. Missing components (w = 1) come from the default-attribute-values address in the shader-state record.
- **Index types:** 8, 16 and 32-bit (`OES_element_index_uint`) map to `v3d_INDEX_TYPE_*`. `OES_draw_elements_base_vertex` maps to `BASE_VERTEX_BASE_INSTANCE`.
- **Primitive restart off:** GLES2 has no primitive restart, so `IndexedPrimList`'s restart enable (a parameter of PiStorm3D's emitter) is always false. An index of 0xFFFF or 0xFFFFFFFF is then an ordinary vertex.

### 7.6 Textures and samplers
- **Start from `v3d_texture.c`:** its layout choice (LINEARTILE / UBLINEAR / UIF ± XOR per level, after Mesa), lazy mip chain, sub-image upload and rename/park are all reusable.
- **Formats:** use the **hardware's native formats and swizzles** instead of PiStorm3D's always-expand-to-RGBA8.

Swizzles use Mesa's notation: X, Y, Z and W select the texel's first to fourth channel; 0 and 1 are constants.

| GL format | V3D data format | Swizzle | Bytes per element |
|---|---|---|---|
| `RGBA/UNSIGNED_BYTE` | `RGBA8` | XYZW | 4 |
| `RGB/UNSIGNED_BYTE` | `RGBA8` (expand on upload) | XYZ1 | 4 |
| `RGB/UNSIGNED_SHORT_5_6_5` | `RGB565` | XYZ1 | 2 |
| `RGBA/UNSIGNED_SHORT_4_4_4_4` | `RGBA4` | XYZW | 2 |
| `RGBA/UNSIGNED_SHORT_5_5_5_1` | `RGB5A1` | XYZW | 2 |
| `LUMINANCE` | `R8` | XXX1 | 1 |
| `ALPHA` | `R8` | 000X | 1 |
| `LUMINANCE_ALPHA` | `RG8` | XXXY | 2 |
| `BGRA_EXT` (`EXT_texture_format_BGRA8888`) | `RGBA8` | ZYXW (R and B swapped) | 4 |
| ETC1 (`OES_compressed_ETC1_RGB8_texture`) | `RGB8ETC2` (ETC2 decodes ETC1) | XYZ1 | 8 per 4×4 block |
| float (`OES_texture_float`, M6) | `RGBA32F` (32-bit TMU return, `sampler_is_32b` key bit) | XYZW | 16 |

The swizzles and format pairs match Mesa's `v3dx_format_table.c`.
- **Tiling is already general.** The CPU tiling code in `v3d_hw.c` handles 1, 2, 4, 8 and 16-byte elements.
- **The 4-byte assumption lives in `v3d_texture.c`** (§4.2): its layout and upload paths must take the element size.
- **Compressed formats** are tiled as 4×4 blocks treated as single elements, as Mesa does (`src/broadcom/common/v3d_tiling.c`, `v3d_resource.c`).
- **Cube maps:** six faces with full mip chains, face stride in the texture state's `array_stride` field, layout per Mesa `v3d_resource.c`. Face selection in shaders comes from the compiler.
- **Non-power-of-two textures:** fully supported by the hardware, so expose `OES_texture_npot`.
- **Wrap modes and filters:** REPEAT, CLAMP_TO_EDGE and MIRRORED_REPEAT; all GL min/mag filters, keeping PiStorm3D's correct `mip_filter_nearest` split. Add anisotropy later (`EXT_texture_filter_anisotropic`).
- **`glGenerateMipmap`:** v1 uses a CPU box filter per level plus upload; later the TFU (the GPU's texture formatting unit; Mesa `v3dx_tfu.c`).
- **`glCopyTexImage2D`/`SubImage`:** v1 flushes, waits and copies on the CPU (PiStorm3D has this); later a tile-buffer store into the texture.
- **Texture units:** report 16 fragment units (Mesa allows 24 samplers, `V3D_MAX_TEXTURE_SAMPLERS`; GLES2 requires 8). Vertex texture units are 0 in v1, which GLES2 allows.
- **Incomplete and unbound textures:** GLES2 requires a sampler whose texture is incomplete to return (0, 0, 0, 1). Incomplete here means, for example, a mipmapping filter without a full chain, a cube map with mismatched faces, or the default texture object 0 with no image. Bind a shared 1×1 texture holding that value for such units at draw time, as Mesa's state tracker does with its fallback texture. Completeness is cached on the texture object and re-evaluated when its images or parameters change.

### 7.7 Framebuffers, passes and jobs
- **A job** is one binning pass plus one render pass for one framebuffer configuration (colour attachment plus depth/stencil attachment). This generalises PiStorm3D's frame (`gl_FrameBegin` → `gl_FramePresent`), including its intermediate-pass machinery, which becomes a special case.
- **Tile pools:** each job needs tile-state and tile-allocation memory sized for its dimensions. They are cached on the framebuffer object (allocated on first use) and the default framebuffer uses the context's.
- **Job lifecycle, v1:**
  - the current job is per context;
  - `glBindFramebuffer` to a different configuration flushes it;
  - flush means submit binning plus render under the arbitration lock;
  - jobs run in flush order.
- **Cost of a flush:** PiStorm3D's submit **waits for binning on the CPU** (§4.1), because binner out-of-memory has to be serviced by polling. Every extra job per frame therefore stalls the 68k for one binning pass. That is acceptable for the usual one or two FBO passes per frame, but it penalises applications that switch framebuffers often.
- **Job lifecycle, v2 (M6):** keep several unflushed jobs at once, keyed by framebuffer configuration as Mesa's job table does, and flush only on hazards, swap or `glFinish`.
  - **The `INCREMENT_SEMAPHORE`/`WAIT_ON_SEMAPHORE` control-list packets work on V3D 4.2** (M1 probe, runs 9 and 10): with the first ending the binning list and the second starting the render list (VC4's protocol), both lists can be submitted at once and the render waits for the binner by itself; the pictures are right. It saved nothing measurable on small frames (200 frames in 72 ms either way), because the CPU still waits for binning to service binner out-of-memory. v2 can use it to keep several jobs in flight, with tile-allocation memory sized generously enough that binner out-of-memory becomes rare.
- **Hazards** (Mesa `v3d_flush_jobs_writing_resource` / `…_reading_resource`):
  1. sampling a texture that is an attachment of an unflushed job: flush that job first;
  2. rendering into a texture that an earlier unflushed job samples: flush the readers first. This only matters once v2 keeps several jobs open; v1's in-order flushing already guarantees it;
  3. CPU modification of a resource a pending job references: rename it (§7.3), or else flush and wait;
  4. `glReadPixels`, `glCopyTex*`, `glFinish`: flush and wait;
  5. `glFlush`: flush without waiting.
- **Colour attachments:** RGBA8, RGB565, RGBA4 and RGB5A1 map to a tile-buffer internal type/bpp and to the store output formats `RGBA8`, `BGR565`, `ABGR4444` and `ABGR1555` (`v3d_output_image_format`, same pairing as Mesa's format table). The store goes into **the texture level's own layout** (UIF/UBLINEAR/LINEARTILE, decided by `v3d_texture.c`), so the result is directly sampleable.
- **Renderbuffers:** RGBA4, RGB565, RGB5_A1, DEPTH_COMPONENT16 (`D16`), STENCIL_INDEX8 (`S8`, stored separately from depth as Mesa does) and, via `OES_packed_depth_stencil`, DEPTH24_STENCIL8 (`D24S8`). Their memory layout follows the same per-level tiling rules as textures (UIF for most sizes, UBLINEAR/LINEARTILE for small ones), as in Mesa. `glCheckFramebufferStatus` follows the GLES2 rules.
- **Stencil:** `STENCIL_CFG` packets per draw (front and back, from `glStencil*Separate`), the clear value in the ZS clear packet, and stencil load/store selectors (`v3d_STENCIL`, `v3d_ZSTENCIL`).
- **Clears:**
  - at job start: a tile-buffer clear (PiStorm3D's `pending_clear_*`);
  - mid-job, scissored or masked: a quad through an internal clear program (PiStorm3D `gl_ClearViaQuad`, rewritten on the new program path).
- **Depth and stencil stores:**
  - **Default framebuffer:** stored only at mid-frame splits (§7.2).
  - **FBOs:** every attachment is stored unless the application invalidates it with `EXT_discard_framebuffer`, because a later pass may read it.
- **Dithering:** `GL_DITHER` (on by default in GLES2) selects the store's dither mode, as PiStorm3D's default-framebuffer store already does. It matters for 16-bit targets (RGB565, RGBA4, RGB5A1).
- **MSAA:** 4× on the tile buffer later (M6+); the hardware supports it at low cost.

### 7.8 Programs and uniforms
- **Shader object:** type, source, compile status, info log.
  - **S1:** `glCompileShader` only records the source and reports `COMPILE_STATUS` TRUE. The real work happens at link, against the cache (below). This is a documented deviation until S2.
  - **`GL_SHADER_COMPILER` must report TRUE in S1 as well.** The GLES2 spec requires `glShaderSource`, `glCompileShader`, `glGetShaderPrecisionFormat` and `glReleaseShaderCompiler` to fail with `GL_INVALID_OPERATION` when it is FALSE, which would disable the source-cache path. A build option for a strict binary-only mode (FALSE, binaries only) can come later.
  - **S2:** compiles for real.
- **Program object:** attached shaders, attribute bindings, link status, info log, and after linking:
  - the loaded `.v3dp` image (§8.3), with shader code uploaded once into a code BO;
  - the **uniform table** (name → location, GL type, array size, storage offset) and **attribute table** (name → location → shader input slot);
  - **sampler bindings** (sampler uniform → texture unit);
  - **uniform storage**: host-order 32-bit words plus a dirty serial.
- **`glLinkProgram`** computes the program hash from the vertex and fragment sources (`v3dp/FORMAT.md`, section `PROGRAM`), then looks it up in order. Attribute bindings are not part of the hash: the runtime maps attributes to the shader's input slots by name, so one binary serves any `glBindAttribLocation`.
  1. in-memory cache;
  2. disk cache in a directory named by an assign (`OGLES2Cache:`), checking `PROGDIR:` first so applications can ship their own binaries. The file name is the hash's first 24 hex digits plus `.v3dp` (29 characters: FFS file names are at most 30); the full hash inside the file must match;
  3. the S2 compiler, if installed;
  4. otherwise it fails with an info log naming the hash, and appends both sources to `T:ogles2_missing_shaders.txt` so they can be compiled offline.
- **Variants:** Mesa compiles each shader per *key* (`struct v3d_vs_key` / `v3d_fs_key` in `v3d_compiler.h`; filled in `v3d_program.c`). The parts that GLES2 state can change:
  - **all stages:** `sampler_is_32b`, set when a sampler returns 32-bit data (float textures);
  - **vertex/coordinate:** `used_outputs` (the varyings the linked fragment shader reads), `is_coord`, and `per_vertex_point_size` (set when drawing points);
  - **fragment:**
    - `is_points`/`is_lines` (primitive class) and `point_coord_upper_left`;
    - `cbufs`, which records whether a colour buffer is bound. It is 0 for a depth-only FBO pass, for example a shadow map;
    - `can_earlyz_with_discard`, which applies to shaders that `discard`, and depends on whether depth/stencil writes are enabled;
    - `msaa` and `sample_alpha_to_coverage` once MSAA exists.

  These fields never change under GLES2: `color_fmt` and `blend` (set only for logic ops and software blending), `f32_color_rb` (32-bit float render targets), `swap_color_rb` (always 0 in current Mesa), `line_smoothing` and `ucp_enables`.

  A `.v3dp` holds one or more variants. At draw time the driver computes the runtime key and picks the matching variant. On a miss it compiles (S2). In S1 it logs the miss and uses the closest variant, so that draw may render wrongly; for example, a triangle variant used for points leaves the point size undefined. The miss log names the key so the variant can be added.

  In practice most GLES2 programs need **one to three variants**:
  - **primitive class** (triangles, lines, points) is the dimension that varies in real applications;
  - **depth-only passes** and **discard with depth writes** add one each where a program is used that way.

  The offline tool covers the common cases by default (§8.2).
- **`glUniform*`** writes storage and bumps the serial. Uniform streams are built at draw time by the recipe writer (§8.6) and memoised per (program, uniform serial, texture-binding serial, viewport serial, control-list generation), like PiStorm3D's vertex-uniform memo.
- **Indirectly indexed uniform arrays** (GLES2 requires any index into vertex-shader uniform arrays, for skinning) reach the driver as `QUNIFORM_UBO_ADDR` index 0; the `skin` test program shows it. Copy the stage's constant buffer (the `.v3dp` `STAGE_PARAMS` layout: every vector, array element and matrix column on 4 values), little-endian, into `state_buf` per change and pass its address.
- **`glGetShaderPrecisionFormat`:** all precisions are fp32 (range 127/127, precision 23); int range 31/30.

### 7.9 Draw path
For each `glDrawArrays`/`glDrawElements`:
1. **Validate:** program linked, framebuffer complete, valid mode and count. GL errors otherwise.
2. **Job:** get or start the job for the current framebuffer. The first draw of a pass emits the per-pass residue (`gl_EnsureDrawState`), except `VCM_CACHE_SIZE`, which moves to step 9.
3. **Hazards** for every texture the program samples (§7.7).
4. **Vertex input:** a buffer address per active attribute (VBO copy, client copy, or stride-0 constant), plus the index buffer.
5. **Texture and sampler state** per used unit (`v3d_texture_emit_state`, with PiStorm3D's texture-state cache keyed on the texture pointer and evicted on rename).
6. **Uniform streams** for the coordinate, vertex and fragment shaders, each written in **one claim** whose size comes from the recipe.
7. **Dirty state packets**, using PiStorm3D's dedup cache (`mglv3d_dedup_cache` pattern):
   - CFG_BITS (cull, front face with the winding inverted for the default framebuffer per §7.11, depth test/func/write, stencil enable, polygon-offset enable, blend enable);
   - BLEND_CFG / BLEND_ENABLES / BLEND_CONSTANT_COLOR, COLOR_WRITE_MASKS, STENCIL_CFG, DEPTH_OFFSET;
   - CLIP_WINDOW (scissor ∩ viewport), clipper XY scaling / Z scale-offset / Z min-max, VIEWPORT_OFFSET;
   - POINT_SIZE and LINE_WIDTH. **Send POINT_SIZE on every draw.** On V3D 4.2 Mesa does this unconditionally as a binner-FIFO erratum workaround (`v3dx_emit.c`). PiStorm3D dedups POINT_SIZE like the other packets, which this workaround does not allow.
8. **Early-Z:** per-frame direction accumulation as in PiStorm3D, plus the program's `disable_ez`/`writes_z`.
9. **VCM_CACHE_SIZE, GL_SHADER_STATE record and attribute records,** filled from the program's prog_data exactly as Mesa `v3dx_draw.c` does (VPM segment sizes, thread count, `uses_center_w`, `lock_scoreboard_on_first_thrsw`, implicit point/line varyings, Z-write and early-Z flags, varying count).
   - The VPM configuration is computed by the driver, not stored in prog_data. It is `v3d_compute_vpm_config` (Mesa `src/broadcom/compiler/vir.c`, a small self-contained function to copy), computed from the coordinate and vertex shaders' VPM sizes and the device's `vpm_size` (§4.2).
   - It supplies the `VCM_CACHE_SIZE` batch counts and the record's "minimum segments required in play" fields. PiStorm3D leaves those fields zero (its record builder clears the record and never sets them). That has worked for its fixed shaders, but the GLES2 path must use Mesa's computed values, because compiled shaders' VPM use varies per program.
   - Cache the result per program, because it depends only on the program.
10. **Primitive packet:** `VertexArrayPrims`, or `IndexBufferSetup` + `IndexedPrimList`. All seven GLES2 modes are native (`V3D_PRIM_*` includes LINELOOP and TRIANGLEFAN).
11. **Bookkeeping:** BO references for the job, texture `last_draw_frame` stamps, statistics.

None of these steps does any per-vertex CPU work on the VBO path. That is the main performance gain over MiniGL.

### 7.10 State, errors, queries and limits
- **State:** full GLES2 state per context (capabilities, blend, depth, stencil front/back, cull, polygon offset, scissor, viewport, depth range, masks, clear values, pixel store, hints, line width, sample coverage) and object name tables (buffers, textures, shaders, programs, framebuffers, renderbuffers). Reuse PiStorm3D's `glGet*` structure.
- **`glGetError`:** first error sticks until read (PiStorm3D's `GLFlagError`). `OGLES2_CCT_SINGLE_GET_ERROR_MODE` is accepted (D9).
- **Strings:** GLES2 fixes the format of two strings. `GL_VERSION` must begin with `"OpenGL ES 2.0"`, and `GL_SHADING_LANGUAGE_VERSION` with `"OpenGL ES GLSL ES 1.00"`; programs parse them. PiStorm3D's `GLGetString` (`others.c`) answers `"1.1"` and is a template only for the mechanism.
- **Private tokens:** `GL_V3D42_PISTORM_BINARY` (§8.3) and any other private enum need values that cannot collide with Khronos ones. Request a vendor enum block from the Khronos registry, or document the chosen values in `<libraries/ogles2.h>` before the first release.
- **Reported limits** (from Mesa `v3d_limits.h` / `v3d_screen.c`):

| Query | Value | Basis / GLES2 minimum |
|---|---|---|
| `MAX_VERTEX_ATTRIBS` | 16 | `V3D_MAX_VS_INPUTS` 64 components / min 8 |
| `MAX_VARYING_VECTORS` | 16 (confirm against Mesa's GLES reporting) | `V3D_MAX_FS_INPUTS`/4 / min 8 |
| `MAX_VERTEX_UNIFORM_VECTORS`, `MAX_FRAGMENT_UNIFORM_VECTORS` | 256 initially (hardware stream allows far more; Mesa const buffer 16K floats) | min 128 / 16 |
| `MAX_TEXTURE_IMAGE_UNITS`, `MAX_COMBINED_TEXTURE_IMAGE_UNITS` | 16 | Mesa allows 24 samplers / min 8 |
| `MAX_VERTEX_TEXTURE_IMAGE_UNITS` | 0 (v1) | min 0 |
| `MAX_TEXTURE_SIZE`, `MAX_CUBE_MAP_TEXTURE_SIZE`, `MAX_RENDERBUFFER_SIZE` | 4096 | `V3D_MAX_IMAGE_DIMENSION`, `V3D_MAX_FRAMEBUFFER_SIZE(42)` |
| `MAX_VIEWPORT_DIMS` | 4096 × 4096 | |
| `ALIASED_POINT_SIZE_RANGE` | 1–512 | `V3D_MAX_POINT_SIZE` |
| `ALIASED_LINE_WIDTH_RANGE` | 1–32 | `V3D_MAX_LINE_WIDTH` |
| `SAMPLE_BUFFERS` / `SAMPLES` | 0 / 0 (4× later) | `V3D_MAX_SAMPLES` 4 |
| `SHADER_COMPILER` | TRUE in S1 and S2 (§7.8) | FALSE would forbid `glShaderSource`/`glCompileShader` |
| `NUM_SHADER_BINARY_FORMATS`, `NUM_PROGRAM_BINARY_FORMATS_OES` | 1 (`GL_V3D42_PISTORM_BINARY`, private token) | |
| `IMPLEMENTATION_COLOR_READ_FORMAT` / `_TYPE` | Default framebuffer: `GL_BGRA_EXT` / `GL_UNSIGNED_BYTE` (`EXT_read_format_bgra`). FBOs: the attachment's own format | GLES2 requires RGBA/UNSIGNED_BYTE plus one implementation-chosen pair, which may depend on the bound framebuffer. BGRA is the RTG screen's native order, so those reads need no reorder |

### 7.11 Framebuffer orientation and state parameters
The runtime replaces Mesa's GL state tracker, so it must also do the state tracker's work that compiled shaders and the hardware set-up rely on.

- **Orientation.** GL puts the window origin at the bottom left. V3D renders with row 0 at the top, and only supports an upper-left fragment-coordinate origin (`fs_coord_origin_lower_left = false` in Mesa's `v3d_screen.c`).
  - **FBOs:** these need no correction. GL's y = 0 lands on texture row 0, which is what a later texture fetch expects.
  - **Default framebuffer (the top-down RTG bitmap):** this is flipped the way Mesa's state tracker flips a window-system framebuffer:
    - negate the viewport's Y scale. This means a negative `QUNIFORM_VIEWPORT_Y_SCALE` value in the uniform stream and a negative half-height in `CLIPPER_XY_SCALING`, with the Y offset measured from the top;
    - invert the front-face winding (CFG_BITS clockwise flag);
    - feed `gl_FragCoord` the y-transform parameter the state tracker adds;
    - choose the point-sprite origin key bit (`point_coord_upper_left`) to match;
    - flip the scissor rectangle (and with it the clip window) the same way;
    - take rows bottom-up in every CPU-side read of the screen (`glReadPixels`, `glCopyTex*`).
  - PiStorm3D already flips the screen. `viewport.c` measures the viewport centre from the top, and its hand-written vertex shaders negate Y. Compiled shaders take the flip from the negative viewport Y scale instead, as in Mesa. Getting either case wrong shows up as upside-down render-to-texture results, swapped culling, or mirrored `gl_FragCoord` effects. §10 therefore tests culling, `gl_FragCoord` and `gl_PointCoord` on both target kinds.
  - **Rejected alternative:** the `Flip Y` field of the Load/Store Tile Buffer General packets. Mesa never uses it; Mesa's XML says the packet's height field is "used for y flip". The M1 probe stored a raster target with `Flip Y` set, the height given and the address of the first row: the store wrote outside the bitmap and the machine crashed (run 8). The viewport flip stays the method; `Flip Y` is not used.
- **Built-in state parameters.** Besides user uniforms, the program's parameter list (written to the `.v3dp` by `v3dglslc`, §8.2) contains state variables the state tracker adds. GLES2's built-in `gl_DepthRange` uniform and the `gl_FragCoord` y-transform above are examples. The runtime evaluates each such entry per draw from GL state; §8.3's `STAGE_PARAMS` tables mark them as state entries with Mesa's state indexes. The loader rejects any state entry it does not know, so nothing is silently left at zero.

---

## 8. Shader compiler strategy

### 8.1 Why Mesa's compiler
- PiStorm3D's assembler, packet definitions and hardware sequences already come from Mesa, and Mesa's V3D compiler (`src/broadcom/compiler`: NIR → VIR → QPU, ~21k lines) is the reference for this exact GPU.
- It handles what a new compiler would have to re-learn: register allocation with thread-count fallback, QPU scheduling (`thrsw` delay slots, `ldvary` timing, TMU FIFO limits), spilling, coordinate-shader generation, and uniform-stream layout.
- GLSL ES 1.00 parsing and linking come from Mesa's GLSL front end.
- The OS4 ogles2 stack makes the same trade-off: its GLSL compiler (about 1.8 MB compressed) is the largest part of that library.

### 8.2 Stage S1: offline compiler `v3dglslc` (host, Linux/WSL)
- **Build (settled 2026-10-10): the S2 code, built natively.** `v3dglslc` is the stream C harness (`dev/spike-c/harness/v3dc.c`) linked on the PC against the host Mesa build's own objects (pinned at `74b6971`, same patches), with the same fake V3D DRM device as the 68k module (`build_v3dc_host.sh`). Two reasons: the writer reads Mesa's internal program tables, which a program calling `libGLESv2` through EGL cannot reach; and S1 and S2 then run the same code, so their `.v3dp` files can be compared byte for byte (they are identical, §13). The drm-shim build (§2.4) stays as the independent reference the spike compared against.
- **Tool:** per input program (two source files, or a manifest that also lists extra key states) it compiles and links, then draws once per key state so Mesa compiles every needed variant. Draws on the fake device compile and then complete without running.
  - **Default key states:** triangles, lines and points into an RGBA8 target with depth writes on. For programs that read `gl_PointCoord`, points into both the default framebuffer and an FBO, because the point-sprite origin bit differs (§7.11).
  - **On request (manifest):** depth-only targets, discard with depth writes off, and float-texture samplers.
  - **Done for the defaults (2026-10-10):** `v3dc -o` draws triangles, lines and points into an RGBA8 + 16-bit depth FBO, each sampler on its own unit with a complete RGBA8 texture of its kind (2D or cube), then puts the samplers back on unit 0. That yields one vertex and one binning shader (two more when the vertex shader writes `gl_PointSize`) and one fragment shader per primitive class.
  - **The link-time variants are not used.** The driver's link-time variants (`V3D_DEBUG=precompile`) have keys no draw computes: the vertex shader keeps every output instead of the ones the fragment shader reads, and a GLES2 `gl_FragColor` shader gets `cbufs = 0xff` (all eight colour buffers, seven extra tile-buffer writes) instead of the bound target's.
  - **Not covered yet:** the default framebuffer's point-sprite origin (the surfaceless tool has no window framebuffer, so `point_coord_upper_left` variants for points into the screen are missing; a miss compiles with S2), and the manifest's extra states.
- **Capture:** `v3dglslc` disables Mesa's on-disk cache (with it on, a program compiled before is loaded instead of compiled, and the hook never runs; found in the stream C spike).
  - **Variants:** Mesa patch 0009 (`dev/spike-c/patches/`, about 20 lines in `v3d_program.c`) adds `v3d_compiled_variant_hook`, called with stage, key, prog_data and QPU code after every compile. While it is set, the driver's in-memory and disk caches are bypassed, so a shader that another program already compiled with the same key is still reported.
  - **Program tables:** read after link from Mesa's `gl_shader_program`, no patch: each stage's parameter list and constant-buffer template (uniforms, constants, state variables, §7.11), the uniform storage (where `glUniform*` writes, sampler units) and the active attributes with their input slots.
  - The writer is `v3dp/v3dp_writer.c`; it serialises every field explicitly, little-endian.
- **Settled: keep the GL path.** Linking Mesa's compiler libraries without GL would mean re-implementing the linker glue the GL path gets for free; the stream C spike showed the full GL path also runs on 68k (§8.4), so S1 and S2 share it.
- **Outputs:** `.v3dp` files (§8.3), an index for the disk cache, and a corpus regression run (shader-db style) for later S2 comparisons.
- **The same harness, run against Mesa's llvmpipe on this PC** (or on the A1200's Pi booted into Raspberry Pi OS), gives reference images (§10).

### 8.3 Program binary container `.v3dp` (little-endian fields)
The format is specified field by field in **`v3dp/FORMAT.md`** (version 1.0, **frozen 2026-10-10** after the draw-time variants and a test program that uses every table, §13). `v3dp/v3dp_dump.py` decodes and checks a file independently of the writer. In outline:
- **Header:** magic `V3DP`, format version, device version (42), Mesa commit, section count, CRC-32 of everything after the header.
- **Sections:** `PROGRAM` (source hash, stages), `STRINGS`, one `STAGE_PARAMS` per stage, `UNIFORMS`, `ATTRIBUTES`, one `VARIANT` per compiled shader. Unknown section types are skipped.
- **Per variant** (one section each for the vertex, binning and fragment shaders of each key):
  - the key, written field by field;
  - QPU code (64-bit LE words, ready to copy), uniform recipe (`count`, then the types, then the data words), and every prog_data field, of which the runtime interprets those Mesa's `v3dx_draw.c` and `v3d_program.c` read:
    - **vertex/coordinate** (`v3d_vs_prog_data`): `vpm_input_size`, `vpm_output_size`, `vattr_sizes[]`, `separate_segments`, `uses_iid`, `uses_biid`, `uses_vid`;
    - **fragment** (`v3d_fs_prog_data`): `num_inputs`, `flat_shade_flags`/`noperspective_flags`/`centroid_flags`, `writes_z`, `writes_z_from_fep`, `disable_ez`, `uses_center_w`, `uses_implicit_point_line_varyings`, `lock_scoreboard_on_first_thrsw`, `force_per_sample_msaa`;
    - **all three** (`v3d_prog_data`): `threads`, `single_seg`, `spill_size`, `tmu_dirty_rcl`.
  - The writer stores the whole prog_data structs anyway (as `v3d_disk_cache.c` does). This list is what the runtime must interpret, and what the format review checks against `v3dx_draw.c`.
- **Program tables:**
  - per stage, the constant buffer: its template and its parameters (uniform, constant, or state variable with its state indexes, §7.11);
  - uniforms (name, GL type, array size, flags for sampler/built-in/hidden, where `glUniform*` writes in each stage's constant buffer with strides, and the sampler index per stage);
  - attributes (name, GL type, input slot).
  - The varying count is per fragment variant (`num_inputs`), not a program table.
- **Integrity:** one CRC over the file. The loader rejects, with an info log:
  - a container version it does not support;
  - a Mesa commit not on the runtime's list of validated commits, so stale cache files are recompiled (S2) or reported (S1) instead of silently used. Binaries that applications ship in `PROGDIR:` keep working across runtime updates as long as their commit stays on the list;
  - any unknown recipe type (§8.6).

### 8.4 Stage S2: on-target compiler module
- **Design (settled by the stream C spike, 2026-10-10): run the offline compiler's own code path on 68k.** The module contains Mesa's GLSL front end, NIR, GL core and state tracker, and the gallium V3D driver, exactly as S1 uses them on Linux, on top of a **fake V3D DRM device** that answers the driver's kernel calls the way Mesa's drm-shim does (V3D 4.2 identification, buffer objects as plain memory, jobs that complete without running). The module never touches the GPU; `ogles2.library` runs the compiled code.
  - **Why this instead of extracting a compiler subset:** no GL linker glue has to be written (uniform, attribute and sampler layout, variant keys and NIR finishing all come from Mesa), and S1 and S2 share every line of compiler and driver code, which is what the bit-identical acceptance below needs. The spike shows the cost is only size (about 19 MB, below).
  - **Sources:** Mesa's `src/util`, `src/c11`, `src/compiler` (GLSL, NIR, SPIR-V front end, which the GL core references), `src/mesa` (GL core and state tracker), `src/gallium/auxiliary`, `src/gallium/drivers/v3d`, `src/broadcom` (compiler, QPU, common, CLE packers, tiling), and driconf without its XML parser. The spike's `dev/spike-c/replay.py` holds the exact list, flags and exclusions (LLVM, KMS, ASTC, Mesa's disk-cache database, the command-list decoder).
- **How:**
  - generate the Python- and flex/bison-generated files on the host;
  - build with a plain Makefile listing the sources, as PiStorm3D does; the spike's replay of Meson's compile commands (`dev/spike-c/replay.py`) already holds the exact source list and flags to generate it from;
  - provide a C11-threads shim on Exec semaphores and the freestanding runtime from `runtime_shim.c`, extended as needed.
- **Packaging:** a separately loaded module (e.g. `v3dglsl.library`) with a C ABI (`compile(manifest) → .v3dp blob`). `ogles2.library` opens it lazily on the first cache miss, and `glReleaseShaderCompiler` closes it. This keeps the core library small and isolates the C++ runtime. Inside, the module creates one V3D screen on the fake device and one surfaceless GLES2 context through the state tracker, as the spike's `v3dc` does, and captures each compiled variant with the same dump code as S1 (§8.2).
- **Build flags:** GCC 16.2, `-mcpu=68020 -msoft-float` (software floating point; see the spike results below for why not the FPU), libnix, `-fno-exceptions -fno-rtti`, plus the defines and the compatibility layer of `dev/spike-c/compat/`.
- **Acceptance:** for the S1 corpus, S2 output must match `v3dglslc` (same Mesa commit). Bit-identical output is the expectation. Any difference must be explained (for example iteration order of pointer-keyed hash sets, which differs between hosts) and must still render identically in the image tests. Results are written to the disk cache.
- **Spike first:** build the GLSL front end alone for m68k, then measure code size, memory and compile time on hardware.
  - It needs only the GCC 16 toolchain (§2.4), not the `.v3dp` format or M2, so it runs during M0/M1 as stream C.
  - **Runtime questions it must answer:**
    - GCC 16 code uses libnix, while the main library uses clib2 (§2.3), so the module cannot share C-runtime state with it. The module's `malloc`/`free`, `printf`-style logging and file access go through a call table the main library passes in at open time. The spike lists every libnix and libstdc++ symbol the front end pulls in and sorts them into "self-contained" (string, math, `qsort`), "redirect to the call table", and "must not be linked" (anything needing a process context: `exit`, `atexit`, stdio streams, constructors that need `__startup`).
    - Static C++ constructors and destructors must run at module open and close, not at program start.
    - Exceptions and RTTI: Mesa's compiler builds with neither, so the module uses `-fno-exceptions -fno-rtti`. libstdc++'s throw helpers are still linked (and need `__xpg_strerror_r`, which libnix lacks). In the full front end libstdc++ adds only about 120 KB.
  - Its result decides D5 before M3 starts.
- **Spike results (2026-10-10, `dev/spike-c/README.md`).** Part 1, front end and NIR:
  - **All 450 source files compile** with GCC 16.2: util, NIR, the GLSL front end and preprocessor, the V3D back end, QPU and common code. Seven files are left out (ASTC, DRM, Mesa's disk-cache database) or replaced (`os_time.c`, `os_misc.c`).
  - **Mesa's standalone GLSL compiler links with no undefined symbols** and runs under the vamos emulator. On four test programs (texturing with `discard`, lighting loops over uniform arrays, skinning with dynamically indexed matrices, constant `exp2`/`log2` folding) **all 17 outputs are identical to the x86-64 build's**: parsing, HIR, LIR and linking behave the same on big-endian 68k.
  - **Size:** 11.2 MB stripped. 7.2 MB of it is one generated table, `nir_opt_algebraic.c`, which is equally large on x86-64. With about 1.9 GB of fast RAM this costs load time (about a second, once), not memory; trimming the rule tables to what V3D uses is the lever if needed.
  - **Rules the port must keep** (each found by the spike):
    - build with GCC 16.2, never 16.1 (§2.3);
    - `uint32_t` defined as `unsigned int` (newlib makes it `unsigned long`; Mesa assumes `int`-sized names match);
    - every allocation 8-byte aligned (Mesa's `ralloc` asserts it; the module's allocator, fed by `ogles2.library`, must provide it);
    - a merge-sort `qsort` instead of libnix's quicksort (a Mesa comparator returned −1 for equal elements and made libnix's partitioning run past the array; fixed in Mesa too);
    - no static object aligned beyond 8 bytes (the hunk format's limit);
    - libm gaps (`log2f`, `exp2f`) filled; `exp2f` through `exp` may differ from glibc in the last bit, which only matters when NIR folds `exp2` of a constant and is covered by the S1/S2 comparison.
  - **Part 2: the whole path, GLSL to QPU code.** Mesa's GL core, state tracker and V3D driver (911 source files in all) compile and link for 68k; on the fake DRM device the test program `v3dc` compiles the four test programs, and **the V3D driver's output is identical to the x86-64 offline compiler's**: all 12 shaders (vertex, binning, fragment), 874 QPU instructions, their uniform streams and the shader-db statistics. Size: 18.8 MB stripped.
  - **Further rules from part 2:**
    - **build the module with software floating point** (`-msoft-float`). NIR's constant folding must round every float operation to single precision, as x86-64 SSE does, or folded constants differ by an ulp from the offline compiler (`mix(0.1, 0.9, 0.25)` folded to `0x3e999999` instead of `0x3e99999a`). With `-m68881` GCC keeps 80-bit intermediates (`FLT_EVAL_METHOD 2`). 68040 code (`fsadd`, `fsmul`) rounds per operation on a real 68040 and in the vamos emulator, but **Emu68 ignores those instructions' precision and computes in double** (`src/M68k_LINEF.c`: the decoded precision is discarded with `(void)precision`), which the A1200 run showed. libgcc's soft-float routines are exact IEEE, independent of the FPU emulation; the compiler is mostly integer code. Worth reporting to Emu68's author. (The runtime library keeps PiStorm3D's `-mcpu=68020 -m68881`; it folds no constants.)
    - **a big-endian bug in Mesa's V3D compiler**, fixed by `patches/0008`: the TMU configuration words are packed as GPU (little-endian) bytes into a `uint32_t` that the code then uses as a number, which swapped the texture index and the config bits on 68k (the §7.4 problem class). Worth sending upstream.
    - the emulator used for host testing (vamos) mis-emulates the 68040's `fssqrt`, so the build calls libm's `sqrtf` (`-fno-builtin-sqrt`; same results).
  - **Mesa patches:** eight small ones, seven of them portability or bug fixes with no effect on x86 (`dev/spike-c/patches/`); 0006 (comparator) and 0008 (TMU packing) are worth sending upstream.
  - **Measured on the A1200 (2026-10-10):** the front end's output is identical to x86-64 on real hardware, and the whole V3D path compiles a program in about 0.2 s after a 1 s start-up (§13).
  - **Confirmed on the A1200 with the soft-float build:** the GPU code of all four programs is identical to x86-64; software floating point costs about 3% of compile time.
  - **Still open (M3 work, not feasibility):** the module shape (memory call table instead of libnix's program startup, one context reused across compiles); and a smaller build (leave out what GLES2 never reaches, starting with the algebraic rule tables).
- **Alternative evaluated on 2026-10-09 and set aside for now: compiling on a spare ARM core.** On a PiStorm, the Amiga runs on the Pi's ARM cores, so the compiler could in principle run as native ARM code beside the 68k JIT. Emu68 v1.1.0-beta.1's source (the version on the test machine) shows what that would take:
  - **No mechanism exists.** CPU0 runs the 68k JIT, CPU2 the PiStorm bus housekeeper, CPU1 the serial log writer (only with the `async_log` boot option) and CPU3 the PowerPC emulation (only when enabled). Otherwise CPU1 and CPU3 sleep in `wfe` forever (`secondary_boot` in `src/aarch64/start.c`). Their interrupt handler serves only the PowerPC timer (`IRQHandler` in `src/aarch64/vectors.c`). There is no 68k-to-native call either.
  - **Emu68 runs the ARM cores big-endian.** The release build is `aarch64_be` (`EMU68_HOST_BIG_ENDIAN`; confirmed on the `Emu68.elf` built here). Native code would therefore also be a big-endian host, with the same Mesa byte-order exposure as the 68k port. Running the worker little-endian is possible only at EL0 with `SCTLR_EL1.E0E` cleared on that core, since the shared page tables are read big-endian.
  - **What helps:** all cores load the same user page table (`mmu_user_L1`), so a worker would see the Amiga's memory at the 68k addresses. Emu68 is MPL-2.0 and builds on this PC (`dev/host-setup/build_emu68.sh`), and its `virt` target runs in QEMU with a visible console, which is where a worker would be prototyped.
  - **What it would take:** an Emu68 patch with a worker loop on a free core, a mailbox published in the device tree, a loader for a position-independent AArch64 image, instruction-cache maintenance, containment of crashes on that core, and a completion signal. The developer would run a custom Emu68 build until the change is accepted upstream by Emu68's author.
  - **Verdict:** not simpler for a start. The extra work is kernel-level, in another project, and hard to debug on a machine without serial capture, while the main gain (a little-endian host, like S1) needs even more of it. Its remaining advantage is speed. Revisit it if the M3 spike shows the 68k compiler is too slow, or if Emu68 gains a facility for running native code.

### 8.5 Fallback if S2 is not feasible
Write a compact GLSL ES 1.00 compiler in C, limited to the subset Appendix A of the GLSL ES spec mandates:
- unrollable `for` loops only;
- if/else converted to predicated (branch-free) code;
- `discard` via `setmsf`;
- TMU memory loads for dynamically indexed vertex-shader uniform arrays.

It would emit through `v3d_qpu_instr_pack` into the same `.v3dp` format, with S1 output as the correctness oracle. Treat this as a contingency; its size is XL.

### 8.6 Uniform recipe types the runtime must implement
Mesa `v3d_uniforms.c` (`v3d_write_uniforms`) is the reference.

| `QUNIFORM_*` | GLES2 need | Value written |
|---|---|---|
| `CONSTANT` | yes | `data` |
| `UNIFORM` | yes | storage word `data` |
| `VIEWPORT_X_SCALE`, `Y_SCALE`, `Z_SCALE`, `Z_OFFSET` | yes | From viewport and depth range. The X/Y scales are the viewport half-extents × 256, the clipper XY granularity on V3D 4.2 (`devinfo->clipper_xy_granularity` in Mesa). Y is negative for the default framebuffer (§7.11) |
| `TMU_CONFIG_P0`, `TMU_CONFIG_P1` | yes | `data` holds the texture unit and low config bits. P0: that unit's texture-state address \| config bits. P1: its sampler-state address (the border-colour variant, as in Mesa) \| config bits, with the unnormalised-coordinates bit set from the sampler |
| `UBO_ADDR` (index 0) | yes (indexed uniform arrays) | address of the LE copy of storage |
| `SPILL_OFFSET`, `SPILL_SIZE_PER_THREAD` | if the compiler spilled | per-context spill BO, `spill_size × 4 × QPU count` bytes as in Mesa `v3d_program.c` |
| `TEXTURE_WIDTH`/`HEIGHT`/`LEVELS`/`FIRST_LEVEL`, `TEXRECT_SCALE_*` | rare in ES 1.00 | texture parameters |
| `LINE_WIDTH`, `AA_LINE_WIDTH` | only with line smoothing lowered | state |
| `BLEND_CONSTANT_*` | only if blending is lowered into the shader | state |
| everything else (images, SSBO, compute, `FB_LAYERS`, `VIEW_INDEX`, `DRAW_ID`, inline UBOs) | no | rejected at load |

---

## 9. Milestones

**Priority: GLSL first.** Runtime GLSL compilation on the Amiga (S2) is the first goal after real shaders work. The on-target compiler is therefore M3, ahead of GLES2 core completeness (M4) and framebuffer objects (M5). Programs that ship or cache their shaders work from M2 onwards.

Sizes, as effort for one experienced developer and with ±50% uncertainty: **S** ≤ 2 weeks, **M** 2–6 weeks, **L** 6–12 weeks, **XL** > 12 weeks. With the team in §2.4, calendar time depends mostly on how quickly builds can be tested on the A1200, so the sizes are a relative guide rather than a schedule.

**Work streams.** Three streams meet only at the `.v3dp` format. With one developer and an AI assistant they interleave rather than run on separate desks. Host-side work (streams B and C, and generated code) continues while hardware results for stream A are pending.

| Stream | Work | Runs on | Starts |
|---|---|---|---|
| A: runtime | M0, M1, the runtime half of M2 (disk loading, recipe writer, uniforms, textures v1, client indices), M4, M5, M6, M7 | This PC (build) + A1200 (test) | Day one |
| B: offline compiler | `v3dglslc`, the `.v3dp` writer and Mesa patch 0009 (the host half of M2); proposes the `.v3dp` format | This PC (WSL, host Mesa build) | Day one |
| C: on-target compiler | M3 | This PC (GCC 16 cross-build) + A1200 | Spike during M0/M1; the module after the `.v3dp` format is frozen (§13) and the S1 corpus exists (M2) |

**Total:** the milestone sizes (M3 taken as 12–24 weeks, M5 as 4–10) add up to about 40–88 weeks of effort, roughly 9–20 person-months with a midpoint near 15.

### M0: Foundations (M)
- **Scope:**
  - Toolchains: GCC 6.5.0b and GCC 16.2 are installed and verified on this PC (§2.4).
  - Set up the repository layout, the generator (§6.6) and the library skeleton copied from `libraryroot`: ROMTag, open/close/expunge, single vector, import library, `OGLES2Open/Close`.
  - Copy PiStorm3D's `backend/` into ogles2's own `libv3dcore.a`, with globals moved into the device object and the device-info additions of §4.2. minigl.library is not touched; sharing the core with it is deferred to M6 (D3).
  - The ogles2-internal submission lock (§7.1 Phase 1, without the minigl side).
  - The file-based log and the `OGLES2Log` command (§10), since the test machine has no serial capture.
  - Our own smoke-test programs (§10), built with the toolchain on this PC. The demo binaries in the PiStorm3D 29.1 release archive (`demos/`: `cube`, `gears`, `library_test`, `ring`, `warp`; §2.4) serve as a quick check that the A1200 set-up works. The source repository has no `demos/` directory, although its README mentions one.
- **Deliverables:** `ogles2.library` with every entry point stubbed; `libogles2.a` and headers; ABI test; first smoke test.
- **Exit criteria:**
  - the library opens and closes on the A1200 with no memory leak;
  - the ABI test passes for gcc (and vbcc, if supported);
  - the stub smoke test runs and reports the V3D identity and device info (`vpm_size`, `qpu_count`).

### M1: First pixels (M)
- **Scope:**
  - The `agl*` lifecycle for all three surface kinds (fullscreen, window, host bitmap), ported from `context.c`'s `vid_*`.
  - Default framebuffer with D24S8, D32F, D16 or no depth (§7.2), Y-flipped per §7.11; job model v1 (default framebuffer only); `glClear*`, `glViewport`, `glScissor`, `aglSwapBuffers`.
  - The **program-object draw path** with a built-in program described by a hand-written prog_data equivalent. Reuse PiStorm3D's `VERTEX_SMOOTH` + `COORDINATE_TEXTURED` + `FRAGMENT_UNTEXTURED_SMOOTH` assembly as the first "built-in `.v3dp`". The coordinate shader is shared with that pairing in PiStorm3D, and its uniform layout (`v3d_my_uniforms`) becomes the hand-written recipe.
  - Client-array attributes and `glDrawArrays`.
  - **Hardware probes,** run once on the A1200 and recorded in this plan:
    - does the attribute record's "fixed" type read GL 16.16 values (§7.5)?
    - do the control-list semaphore packets let a render queue behind its binning (§7.7)?
    - does the tile-buffer `Flip Y` store/load work for raster and UIF targets, as an alternative to the viewport flip (§7.11)?
    - confirmation of §2.3's memory analysis: draw from buffers in Zorro II RAM and at the top of Pi RAM, and check that `MEMF_FAST` never returns memory outside the device-tree ranges.
- **Exit criteria:** a smooth-shaded triangle in all three surface kinds; a 10,000-frame soak with zero leak (`BytesAllocated` back to 0) and no GPU timeouts; probe results written into this plan.
- **Probe results (A1200, runs 8 to 10, 2026-10-10):** the fixed type reads 16.16 (§7.5); the semaphore packets queue a render behind its binning (§7.7); `Flip Y` on a raster store wrote outside the bitmap and crashed the machine (§7.11); the GPU reads vertex data at the top of the Pi's RAM and in the 512 KB fast region at `0x00C00000`, which lies outside the device tree's ranges; there is no Zorro II RAM on the test machine; GPU memory is now taken only from fast RAM inside those ranges (§7.3).

### M2: Real shaders (L)
- **Scope:**
  - `v3dglslc` and its `.v3dp` writer (stream B), plus loading `.v3dp` files from disk. The format itself and the in-memory loader already exist from M1.
  - `glProgramBinaryOES`/`glGetProgramBinaryOES`, `glShaderBinary`, the source-hash disk cache and the miss log.
  - The recipe writer (§8.6), `glUniform*`, `glGetUniformLocation`/`glGetAttribLocation`/`glBindAttribLocation`, `glGetActive*`.
  - 2D textures v1: RGBA8, RGB8, L, A and LA with swizzles, all filters and wraps, uploaded mip levels.
  - `glDrawElements` with client indices.
- **Corpus:** SDL2 `testgles2` shaders, es2gears, Khronos/OpenGL ES 2.0 Programming Guide samples. This corpus also becomes M3's acceptance set.
- **Exit criteria:** those programs run on the A1200 from cached binaries; uniform and attribute queries return correct values; images match the reference images (§10) within tolerance.

### M3: On-target GLSL compiler (XL; the priority after M2)
- **Scope:**
  - Spike (already during M0/M1, stream C): build Mesa's GLSL front end with GCC 16 for m68k; measure code size, memory and compile time on the A1200.
  - Go/no-go decision between S2 and the fallback (§8.5), which settles D5.
  - The compiler module, lazy loading, disk-cache writing, and mapping `OGLES2_CCT_DEBUG_SHADER_LOG` to info logs.
- **Exit criteria:**
  - corpus output matches `v3dglslc` under §8.4's acceptance rule (bit-identical, or explained differences that render identically);
  - a compile-time target is met (set after the spike, for example under 1 s per typical program);
  - programs that compile shaders at runtime work without a pre-filled cache;
  - `GL_SHADER_COMPILER` behaviour and compile/link info logs are real, no longer the S1 deviation of §7.8.

### M4: GLES2 core complete (L)
- **Scope:**
  - VBO/IBO: the core part (byte-order copies per layout, renaming) was brought forward to M2 (§13); M4 adds range re-swaps on `glBufferSubData`, `OES_mapbuffer`, `OES_element_index_uint`.
  - All primitive types; full blend, depth, stencil, cull, polygon offset, masks, sample coverage (accepted), `gl_PointSize`/`gl_PointCoord`.
  - Textures: 16-bit formats, cube maps, non-power-of-two, `glTexSubImage2D`, `glCopyTex*`, CPU `glGenerateMipmap`, ETC1, BGRA8888, incomplete-texture fallback.
  - All `glGet*`/`glIs*`, `glReadPixels`, `glPixelStorei`, `glHint`, `glFinish`/`glFlush`, strings and extension string.
  - `BRANCH` chaining for the binning list and single-claim uniform streams, so no frames are dropped.
- **Exit criteria:**
  - all 142 core entry points implemented (Appendix A ticked);
  - the in-house functional test suite passes;
  - a stress test that overflows the initial 4 MB binning buffer and the state buffer within one frame shows no dropped frames;
  - the VBO path beats MiniGL on an equivalent gears scene.

### M5: Framebuffer objects (M–L)
- **Scope:**
  - Framebuffer and renderbuffer objects with completeness rules; texture-level and cube-face attachments.
  - All renderbuffer formats including `OES_packed_depth_stencil`.
  - Multi-job frames, per-FBO tile pools, hazard flushing, `EXT_discard_framebuffer`, `glReadPixels` from FBOs.
- **Exit criteria:** render-to-texture, post-processing chains and depth-renderbuffer scenes match reference images; the §10 orientation tests pass on both target kinds; no leaks when FBOs are created and destroyed every frame.

### M6: Ecosystem, sharing and performance (L)
- **Scope:**
  - **SDL2_68k / SDL3_68k:** an ogles2 GL-context backend modelled on `external/reference/sdl-os4/src/video/amigaos4/SDL_os4opengles2.c`, plus SDL's GLES2 renderer.
  - **gl4es:** port its `agl` glue to OS3 ogles2. This needs the M3 compiler, because gl4es generates shaders at runtime.
  - **minigl.library coexistence (D3):** agree with the PiStorm3D maintainers on sharing `libv3dcore` and the submission lock (§7.1), rebuild minigl.library on it, and check that its demos render as with 29.1.
  - **Presentation:** double/triple buffering investigation, vsync, CPU/GPU overlap.
  - **Hardware units and speed:** TFU for mipmaps and uploads, uniform-stream memoisation, optimised swap loops.
  - **Sharing:** the shared submission component (§7.1 Phase 2), multiple contexts per application and `OGLES2_CCT_SHARE_WITH`.
  - **Extensions** (Appendix B.3).
- **Exit criteria:**
  - SDL `testgles2` and renderer tests pass;
  - gl4es glxgears and Quake run;
  - glmark2-es2 runs;
  - a MiniGL game and an ogles2 application run concurrently;
  - frame-time targets are documented.

### M7: Conformance and release (M)
- **Scope:**
  - Run the dEQP-GLES2 functional groups that matter (shaders, fragment ops, FBO, texture). Either port the runner or drive extracted cases from a custom harness.
  - Fix pass.
  - Licence decision (D6) settled before publishing.
  - Release: SDK (headers, import library, examples, autodoc), LHA package, documentation of deviations (unsupported tags, any remaining limits).
- **Exit criteria:** published release with conformance results and known issues.

**Order of work:** M0 → M1 → M2 → M3 → M4 → M5 → M6 → M7.

**In parallel:**
- stream B from day one, so `v3dglslc` is ready when M1 ends;
- the M3 spike during M0/M1, so the go/no-go result is known before M3 starts;
- generated code, tests and host-side tooling whenever hardware results are pending.

---

## 10. Testing, debugging and tooling
- **Our own test programs.** There are no PiStorm3D demo sources to reuse, so the project writes its own small programs from M0 on. They grow into the functional suite.
  - They are written against plain GLES2 plus a tiny EGL/agl shim, so the same source builds for the Amiga and for Linux.
- **Reference images:**
  - **Everyday:** the Linux build runs on this PC under Mesa's llvmpipe (software rendering, built alongside the V3D driver, §2.4) and writes golden images.
  - **On the Amiga:** the same programs render into an `OGLES2_CCT_BITMAP` context and write PPM files (like `MGLWriteShotPPM`).
  - **Comparison:** a host script compares the two with per-test tolerances. llvmpipe and V3D are both conformant but round differently at edges and in filtering, so the tolerances start loose and tighten where a test allows.
  - **V3D-exact references when needed:** the Pi in the A1200's PiStorm can boot Raspberry Pi OS from a spare SD card (or USB) instead of Emu68. The Linux build then runs on the same GPU under Mesa's real `v3d` driver. Use it when a difference against llvmpipe needs a hardware answer.
- **Host unit tests** for the generator, `.v3dp` parser, recipe writer, layout-signature swapping and texture tiling.
  - Keep Hyperion-licensed code out of host tools (§11).
  - The tiling and recipe code meant for host tests should be new MIT code, or cleared with the authors.
- **Control-list dumps:** write submitted control lists and BOs to a file and decode them on the host with Mesa's `src/broadcom/cle/v3d_decoder.c` / CLIF tooling.
- **QPU disassembly:** `v3d_qpu_disasm` is compiled into PiStorm3D (`v3d_assembler.h`), but its text output goes through `v3d_vsnprintf`, which `v3d_assembler.c` stubs to do nothing. So it currently produces empty strings.
  - **Debug builds** link a real `vsnprintf` for it.
  - **Release builds** dump the raw 64-bit words, to be disassembled on the host with Mesa.
- **Debug controls:** `ENV:OGLES2/Debug` flags (log draws, dump shaders and control lists, disable caches, force a flush per draw, verify GPU buffer copies against the shadow).
- **Logging without a serial cable.** The test machine's serial output cannot be captured (§2.4), so every log must end up in a file the developer can forward:
  - The library keeps its log in a RAM ring buffer and appends it to a file (proposed `T:ogles2.log`, path settable in `ENV:OGLES2/LogFile`) at context destroy, at library close and on request from a small `OGLES2Log` command. A program that hangs the machine loses the unflushed tail, so debug builds flush after every line.
  - `kprintf` output (PiStorm3D's `D()`/`E()` macros call exec's `RawPutChar`) can be captured on the Amiga itself with **Sashimi** (Aminet `dev/debug/Sashimi.lha`, v1.7), which patches `RawPutChar` and can save to a file. **Confirmed on the test machine:** in the baseline run, Sashimi captured all 66 test lines `sysprobe` sent through `KPrintF`.
  - Test programs write their results and PPM images to files as well, never only to the console.
  - **`stderr` does not follow a shell redirection on AmigaOS:** libnix sends it to the console (seen in the spike-2 run). Anything meant for a result file goes to `stdout` or to an explicitly opened file; Mesa's own log output inside the compiler module goes through `os_log_message`, which the module routes to the library's log.
- **Counters:** PiStorm3D's `g_mglv3d_*` statistics (spills, state-buffer growth, dropped frames, renames, retire leaks), exposed per context through a debug call.
- **Soak and leak tests:** create and destroy contexts, FBOs and textures in loops while checking device `BytesAllocated`.
- **Orientation tests (§7.11):** the same scene drawn into the default framebuffer and into an FBO, then sampled. Check culling with both windings, a `gl_FragCoord` gradient, `gl_PointCoord` sprites and `glReadPixels` from both target kinds.
- **ABI tests:** the generated table-order and size checker runs on every build.

### 10.1 Remote test station (`amictl`)
**Purpose.** Every hardware result so far has needed the developer to copy a package to the A1200, run it and send the results back. A small server on the A1200 and a client on this PC let the build machine do that itself: upload, run, collect output, logs and screenshots, in seconds and as often as needed. The developer only steps in to press reset after a hard hang, and to watch where a person's eyes are wanted. Decided in D15.

**The network (from the developer, 2026-10-10):** the A1200 is `192.168.1.128` on the local network, with Roadshow on the Pi 4's Ethernet (Emu68's `genet.device`); it answers ping from this PC in about 1 ms, and no other server runs on it. The server may start at boot and stay running.

**Components** (repository `tools/amictl/`, MIT, our own code):
- **`amictld`**, the Amiga server: C, GCC 6 + libnix, `bsdsocket.library` (Roadshow's; nothing Roadshow-specific is used, so AmiTCP and Miami would work too). One small program, started from `S:User-Startup`.
- **`amictl`**, the PC client: Python 3, standard library only. Used by hand and by scripts.
- **A host build of the server** (the same protocol code over POSIX sockets, with stubs for the Amiga-only commands) so the protocol and the client are tested on this PC before anything reaches the A1200.

**Protocol** (TCP port 6800, one client at a time; all numbers big-endian):
- Request: magic `AMC1`, command (u32), argument length (u32), data length (u32), the argument (text), the data (bytes). Response: magic, status (u32, 0 = ok, otherwise an error code), length (u32), the data.
- The first request on a connection must be `HELLO` with the token; anything else, or a wrong token, closes the connection, and the attempt is logged.
- Size limits on every field; malformed input is answered with an error, never trusted.

**Commands:**

| Command | Does |
|---|---|
| `HELLO token` | Authenticates; returns server version, boot count, uptime |
| `INFO` | Kickstart/Workbench and Emu68 versions, free memory by type, loaded libraries (name, version, open count), screens and windows, tasks |
| `PUT path` + data / `GET path` | Write or read a file (written to `path.tmp`, then renamed, so a broken transfer leaves no half file) |
| `LIST dir`, `DELETE path`, `MAKEDIR dir` | Directory housekeeping |
| `START dir command` / `WAIT job timeout` / `ABORT job` | Runs an AmigaDOS command line (several lines allowed) in its own shell with a 200 KB stack; `WAIT` returns the exit code and the output (capped at 1 MB) once it has finished; `ABORT` sends Ctrl-C |
| `run` (client) | `START`, then `WAIT` up to the timeout; on timeout `ABORT` and a grace period; reports a job that still runs as hung |
| `SHOT [screen]` | The front screen (or a named one) as width, height and RGB bytes, read with `ReadPixelArray`; the client writes a PNG, which the assistant can look at |
| `ENV name [value]` | Read or set `ENV:` variables (log paths, debug switches) |
| `REBOOT` | `ColdReboot()` after a delay, for recovery when the server still answers |
| `RESTART` | Starts a newly uploaded `amictld` and exits: self-update without touching the Amiga |

**Running commands safely:**
- A command runs in its own process (a launcher process calls `SystemTags` with output to a file and reports the exit code by signal), so the server's main loop (`WaitSelect` on the socket plus the job and timer signals) keeps answering during a test.
- On timeout the command's process gets Ctrl-C; if it does not end within a grace period the job is reported as hung and left alone (AmigaOS cannot kill a process safely).
- The server and its launchers set `pr_WindowPtr = -1`, so a DOS requester ("please insert volume") can never block the station waiting for a click.
- Each job's output goes to its own file under `T:`, read back and deleted after `WAIT`.

**Security:**
- A 128-bit random token, generated on this PC when the installation package is built, stored next to the server on the Amiga, required on every connection.
- Connections only from the addresses in `amictld.allow` (this PC); everything else is refused before reading any data.
- Every command is logged on the Amiga (`amictld.log`, with time, client, command and result), so the developer can see exactly what was run.
- Port 6800 must not be forwarded from the internet; the station is a LAN tool.
- The assistant says in its messages what it is about to run before running anything that changes the Amiga's system files or reboots it.

**Start, hangs and recovery:**
- `S:User-Startup` runs `amictld` in the background. Until Roadshow is up it retries for up to two minutes, so the startup order does not matter.
- **A hard GPU hang stops the server too**, and only a reset helps. After the reset the server comes back by itself; `amictl wait` polls until it answers, and the boot count shows that a reboot happened. A remotely switchable power socket would remove even that manual step (optional, later).
- A failed test can leave a context, a bitmap lock or the GPU lock behind; `INFO` and the ogles2 log show it, and a reboot clears it.

**Workflow:**
- `amictl runpkg tests/a1200/<package>` uploads the package into the server's own work directory (`PROGDIR:work/<package>`, wherever the server is installed), deletes the results of an earlier run there, expunges every library nobody has open (`Avail FLUSH`, so a library left in memory by an earlier run cannot stand in for the package's), runs its script, fetches `results/` into `tests/a1200/results/<package>-<date>/`, and warns when `ogles2.log` does not start with the library's `init` line. The packages stay what the developer runs by hand too.
- Single tests: upload the library and a test binary, `RUN` it, `GET` the log, `SHOT` the screen.
- Watchable tests (1 s per state, the Shell says what to look for) keep that pacing when the developer is watching; scripts can pass a `QUICK` switch when nobody is.

**Installation (once, by hand):** a package `ogles2-amictld-1` with the server, its token and allow files, and a `ReadMe`: copy the directory to a permanent place (on the test A1200: `Stuff:amictld`; it has no `Work:` volume), add one line to `S:User-Startup` (`Run >NIL: Stuff:amictld/amictld`), reboot. Uninstalling is deleting that line and the directory.

**Steps:**
1. Protocol and server core, the PC client, and the host build of the server; every command tested on this PC.
2. The Amiga build and the installation package; the developer installs it.
3. First remote session: `HELLO`, `INFO`, file transfer both ways, `RUN`, `SHOT`, `REBOOT` and the automatic restart, checked one by one.
4. `runpkg` and `wait`; then M1 continues remotely, starting with `ogles2-m1-4`.

**Risks:** the server is new code running permanently on the test machine, so it is kept small and conservative (no shell, no command other than the table above, size limits everywhere); a bug in it is an annoyance, never a risk to the developer's files beyond what `PUT`/`DELETE` allow, which only the token holder can use.

---

## 11. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| S2 (Mesa compiler on 68k) is too large, slow or hard to port | Programs that compile at runtime need precompiled caches; gl4es is blocked | **Retired by the spike** (§8.4, §13): the whole path builds, matches x86-64, and compiles a program in about 0.2 s on the A1200 after a 1 s start-up. Size about 19 MB, acceptable with the test machine's RAM, trimmable |
| Mesa updates break the variant hook or change the structs the writer reads | S1 and S2 maintenance | Pin the Mesa commit and record it in `.v3dp`; patch 0009 is about 20 lines; the writer's `static_assert`s catch changed array sizes, but a new key or prog_data field is only caught by diffing `v3d_compiler.h`'s structs against `FORMAT.md` at each Mesa update |
| Variant key mismatch at runtime (S1) | Wrong rendering for unusual formats | Multi-variant containers; keys designed around GLES2 defaults; log misses |
| Byte-order bugs | Silent corruption | §7.4 rules; mixed-layout tests; debug verify mode |
| GPU hang (polling, no interrupts, no MMU) | Hung or crashed machine | Timeouts and diagnostics from PiStorm3D; address validation in debug builds; investigate a V3D reset path |
| Concurrent use with minigl.library | Corruption or hangs | Until M6: documented as unsupported, with a warning when minigl.library is open. In M6: shared lock and core, then the shared submitter, with the maintainers' agreement (D3) |
| RTG presentation quirks (`ChangeScreenBuffer` stutter, mixed P96 versions, BGRA32-only, DepthIncludesAlpha) | Stutter or black screens | Reuse PiStorm3D's proven paths; dedicated M6 investigation; FAQ |
| Missed cache flushes (AllocVec memory) | Intermittent garbage or hangs | Flushes are only done at submit, driven by BO dirty ranges (§7.3) |
| Mixed licences (Hyperion: AmigaOS only, publish modifications; Mesa: MIT) | Distribution and host-tool constraints | Host tools stay Hyperion-free from the start; the licence of new code (D6) is settled before release (M7); publish all sources |
| Slow test loop: every hardware result needs a manual copy-and-run on the A1200 | Calendar time, not effort | Manual copying now, a network share later (§2.4); no serial capture, so file-based logs from the start (§10); batch several probes per run; keep host-side work (streams B/C, generator, tests) independent of pending results |
| 68k CPU cost per draw (uniforms, state, client arrays) | Lower than expected frame rates | VBO path with no per-vertex CPU work; memoisation; dedup; profiling counters |
| OS4 tag semantics differ from guesses (OS4 SDK is proprietary) | Porting friction | Document deviations; validate with real OS4-origin code (SDL, gl4es) |
| The GCC 16 toolchain mis-compiles or lacks runtime pieces Mesa needs (C++ runtime, threads, `<stdatomic.h>`) | S2 blocked or unstable | **Happened:** GCC 16.1 miscompiled (callee-saved registers not saved, §2.3); GCC 16.2 (`amiga16.2`) does not. The assembly check (`dev/spike-c/scan_asm.sh`) runs after every toolchain update, and the S1/S2 output comparison catches wrong code the check cannot see. libstdc++ links against libnix with one shim, and threads are needed only as the C11-threads shim of §8.4 |
| Toolchain wrong-code in the runtime library (GCC 6.5.0b) | Subtle runtime faults | GCC 6.5.0b is PiStorm3D's verified compiler (§2.3) and passes the same register-save check; keep it pinned |
| GPU memory allocated outside Pi RAM (chip RAM fallback when fast RAM runs low) | GPU reads or writes unrelated memory; corruption or crash | `MEMF_FAST` plus a check against the device-tree memory ranges for every buffer object (§2.3, §7.3); ranges and allocation addresses measured in the baseline run (§13) |
| CPU waits for binning on every job flush | Multi-pass applications (FBO ping-pong) lose CPU time per pass | v1 accepts it; v2 job table and semaphore-packet probe (§7.7) |
| A crashed client holds the arbitration lock or leaves contexts alive | Other GPU users hang; GPU memory leaks | Lock acquired with timeout; per-task context tracking and cleanup in `LibClose` (§7.1, §7.3) |
| S1 cache misses a variant actually used at runtime (unusual primitive or pass type) | Wrong rendering until the tool is re-run | Default key states cover primitive classes; manifest for the rest; miss log names the key (§7.8, §8.2) |
| State-tracker work the compiled shaders assume is missed or done differently (orientation, winding, scissor flip, state parameters such as `gl_DepthRange`, sampler-to-unit mapping) | Upside-down or mirrored output, wrong culling, uniforms silently zero | §7.11 lists the duties; the loader rejects unknown state entries; orientation tests on both target kinds (§10); compare against Mesa reference images (llvmpipe, or real `v3d` on the A1200's Pi) |

---

## 12. Decisions

| # | Decision | Status | Recommendation or outcome |
|---|---|---|---|
| D1 | Library name | Open | `ogles2.library` (same as OS4; source-compatible API) |
| D2 | Current-context model | Open | Client-side per program image, context passed as the first table argument (§6.2) |
| D3 | GPU sharing with minigl.library | **Deferred to M6** | Not needed until ogles2 works. Then: shared `libv3dcore` plus the submission lock, later the shared submitter; needs the PiStorm3D maintainers. Until then the two must not run concurrently (§7.1) |
| D4 | Default depth format | Open | D24S8 when stencil is requested; otherwise D32F (current MiniGL default), or D16 when 16 bits are requested; no depth/stencil memory when both are 0 (§7.2) |
| D5 | Compiler route | **Decided** (confirmed on the A1200: GPU code identical to x86-64, about 0.2 s per program) | S1 (offline, Mesa) first; S2 as a 68k module running Mesa's own GL + V3D driver path on a fake DRM device (§8.4), which the spike showed builds and matches x86-64 exactly. Fallback compiler (§8.5) only if the A1200 timings make S2 unusable. Compiling on a spare ARM core was evaluated and set aside for now (§8.4): it needs an Emu68 kernel change and gains little while Emu68 runs big-endian |
| D6 | Licence for new code | **Deferred to M7** | Settle before the first public release. Recommendation: MIT for new front-end and tool code; the backend keeps its existing licences (needs Dennis van der Boon's and Steffen Haeuser's agreement). Host tools stay free of Hyperion-licensed code from the start, so either outcome works |
| D7 | Repository | **Decided (2026-10-10)** | New local git repository `this repository` (not published yet): `v3dp/`, `tools/v3dglslc/`, `v3dcore/`, `ogles2/`, `client/`, `include/`, `gen/`, `tests/`, `dev/host-setup/`. This workspace keeps the plan, the release archive, the references and the spike record |
| D8 | Backend pluggability | Open | Keep `backendFlags` in the table, as minigl does |
| D9 | `OGLES2_CCT_SINGLE_GET_ERROR_MODE` and other OS4-specific tags | **Postponed** | Accept every tag; implement what is meaningful (Appendix B). Exact OS4 semantics are not needed before M1 |
| D10 | Default-framebuffer depth/stencil after `aglSwapBuffers` | Open | Undefined, as EGL specifies, so nothing is stored at swap; a PiStorm-specific tag opts into preserving them (§7.2) |
| D11 | Priority among GLSL, FBOs and SDL | **Decided: GLSL first** | Runtime GLSL compilation on the Amiga is M3, ahead of GLES2 completeness (M4) and FBOs (M5) |
| D12 | Team | **Decided** | One developer plus the AI assistant (§2.4) |
| D13 | Test and reference machines | **Decided** | A1200 with PiStorm for all hardware runs; this PC for builds, stream B and llvmpipe reference images (§10) |
| D15 | Remote test station | **Decided (2026-10-10)** | A server on the A1200 (`amictld`, Roadshow on `genet.device`, `192.168.1.128`, started at boot) and a PC client (`amictl`) upload, run and collect tests and screenshots over the LAN (§10.1). Token plus address check; every command logged on the Amiga |
| D14 | Licence | **Decided (2026-10-10): accept the Hyperion terms** | PiStorm3D's backend is under the Hyperion MiniGL Open Source License (its `Licence.txt`; only parts are MIT: Macoy Madson's and the Mesa-derived code). `libv3dcore` copies it, so `ogles2.library` as a whole is distributed under the Hyperion licence: AmigaOS only, and the source of all modifications published to the Amiga developer community. Third-party parts keep their own licences and notices. The library glue is written fresh (not copied from minigl's `libraryroot`), and stand-alone parts that incorporate no MiniGL code (`v3dp/`, `tools/v3dglslc/`, the generator) are MIT |

---

## 13. Status and next steps

**Done (2026-10-09):**
- PiStorm3D driver and minigl.library glue sources gathered and analysed (§3, §4).
- Emu68's memory and cache behaviour established from its source (§2.3). The memory side is now also measured on the test machine (below); `CachePreDMA` still gets its confirmation run in M1.
- Build machine set up in WSL Ubuntu 24.04 (§2.4): package prerequisites, Mesa at `74b6971` with `v3d`, drm-shim and llvmpipe, the WSL1 drm-shim patch, and a passing stream-B smoke test.
- **GCC 6.5.0b toolchain installed and verified** (§2.3, §2.4): clib2 needed `clib2-ndk32.patch`, and libpthread (not used by PiStorm3D) needed `pthread-timersub.patch`. The 29.1 driver built here matches the code in the shipped 29.1 `minigl.library`. This replaces the planned A1200 comparison run with a host-side proof.
- **PiStorm3D 29.1 release archive** fetched from Aminet (§3): shipped library, demo binaries, SDK, `Emu68DeviceTree`.
- Finding: the published library glue cannot link the 29.1 driver by itself (four missing libc functions, §3), so a rebuilt `minigl.library` is not available as a baseline. The shipped one is the baseline.
- **GCC 16 toolchain installed** with libnix and libstdc++. libstdc++ is only built after libpthread, so it needed `pthread-timersub.patch`. A C++17 test program (`<optional>`, `<string_view>`, `<unordered_map>`, structured bindings, `-fno-exceptions -fno-rtti`) compiles and links into an Amiga executable; libstdc++ needs one symbol libnix lacks, `__xpg_strerror_r`, supplied by a 10-line shim. (First built from `amiga16.1`; replaced by `amiga16.2` on 2026-10-10, see below.)
- **Test machine facts** from the developer (§2.4): Pi 4B, Emu68 v1.1.0-beta.1 (exactly the source read for §2.3), manual file copying for now and a network share later, no serial capture (logs go to files, §10).
- **ARM-core compilation evaluated** (§8.4) and set aside for now: Emu68 has no way to run native code on its idle cores, and it runs the ARM cores big-endian. Emu68 v1.1.0-beta.1 builds from source on this PC, which keeps the option open.

**Done (2026-10-10): A1200 baseline run 1** (package `tests/a1200/out/ogles2-baseline-1.lha`, built by `a1200/build_package.sh` from `a1200/baseline-1/`; results in `tests/a1200/results/run1/`). All nine steps finished, no crash, every program returned 0.
- **The GPU set-up works.** PiStorm3D 29.1's five demos ran: `library_test` reports "MiniGLV3D/Broadcom V3D 4.2 b97"; `cube`, `gears` (about 490 fps) and `warp` were perfect. `ring` was almost perfect: the inner rings' polygons disappeared while it rotated. This is a PiStorm3D 29.1 behaviour to keep in mind, because ogles2 copies its backend; re-check it once ogles2 draws similar geometry, and report it upstream if it reproduces.
- **Logging without serial works:** Sashimi captured all 66 `KPrintF` test lines (§10).
- **Memory measured** (§2.3): the device-tree memory node is `/memory@0`, its ranges contain every fast-memory region and every test allocation, and `/v3dbus`'s `dma-ranges` is an identity mapping. Two plan statements were corrected as a result: the `dma-ranges` wording, and the claim that Zorro II RAM falls inside the published ranges (§7.3).
- **Machine details** (§2.4): AmigaOS 3.2 (Kickstart 47.115, Workbench 47.5); Emu68 1.1.0-beta.1, variant `pistorm32lite`, built from `e2f1b9c`, which is the `1_1_wip` head merged without changes as tag commit `0ba3a89` (identical source); Picasso96API 2.488, emulation.library 41.570, rtg.library 43.787, VideoCore.card 1.5; about 1.9 GB fast RAM; PiStorm3D 29.1 was already installed before the run.

**Done (2026-10-10): stream C spike, first part** (`dev/spike-c/`, results in §8.4; whole spike reproducible with `dev/spike-c/run_all.sh`):
- Mesa's compiler code (450 files: util, NIR, GLSL front end, V3D back end) compiles for 68k; the standalone GLSL compiler links and runs, and its output on four test programs is identical to x86-64's in all 17 comparisons. Size 11.2 MB, dominated by one generated rule table that is as large on x86-64.
- **The spike found a wrong-code bug in GCC 16.1** (callee-saved registers not saved) and moved the compiler module to **GCC 16.2** (`amiga16.2`, `134541b3686`), which is clean, as is GCC 6.5.0b (§2.3). Without the spike this would have surfaced as random crashes deep in M3.
- It also found and fixed: Mesa's assumption of 8-byte-aligned `malloc`, a Mesa comparator bug exposed by libnix's `qsort`, `uint32_t` being `unsigned long` on this target, two 32-bit-alignment assumptions, and the hunk format's 8-byte limit for static objects.

**Done (2026-10-10): stream C spike, second part** (`dev/spike-c/harness/`, results in §8.4):
- Instead of hand-written GL linker glue, Mesa's own GL core, state tracker and V3D driver run on 68k over a fake DRM device (911 source files). The V3D driver's output for the four test programs is **identical to the x86-64 offline compiler's**: 12 shaders, 874 QPU instructions, uniform streams and shader-db statistics. This settles the S2 design (§8.4) and D5, pending the A1200 timings.
- Found and fixed: a big-endian bug in Mesa's V3D compiler (TMU configuration packing, `patches/0008`), the need for per-operation single-precision rounding so constant folding matches x86-64 (first tried with 68040 FPU code; after the A1200 run, software floating point, see below), Mesa's on-disk shader cache hiding compiles from the dump (§8.2), and an emulator (vamos) bug in `fssqrt`.
- A1200 packages ready: `tests/a1200/out/ogles2-spike-1.lha` (front end only) and `tests/a1200/out/ogles2-spike-2.lha` (whole V3D path, with every program's GPU code saved for `dev/spike-c/harness/check_a1200.sh`).

**Done (2026-10-10): A1200 spike runs** (results in `tests/a1200/results/ogles2-spike-1` and `ogles2-spike-2`). Every run finished with exit code 0, no crash.
- **The GLSL front end matches x86-64 on real hardware:** all 11 outputs of spike-1 are identical (`dev/spike-c/check_a1200_spike1.sh`). Each run takes about 0.58 s, almost all of it loading the 11 MB program and building Mesa's built-in function table; compiling and linking adds 0.03 to 0.07 s.
- **The whole V3D path is fast enough:** start-up (loading 19 MB, creating the GL context) 1.06 s; one program including start-up 1.21 to 1.35 s; all four programs in one run 1.77 s, so **about 0.2 s of compile time per program** (GLSL to vertex, binning and fragment QPU code) under Emu68. The module pays start-up once per session, so S2 is usable for programs that compile shaders at load time. This settles the speed half of D5.
- **The spike-2 GPU code was not captured:** on AmigaOS libnix sends `stderr` to the console, not into the shell's `>file` redirection (vamos merges them), and both the test program and Mesa's log write to `stderr`. Fixed (`stderr` is pointed at `stdout` at start-up); the corrected package is rebuilt. The test logs and the library's log (§10) must never rely on `stderr` reaching a redirected file.
- Free fast memory fell by about 0.36 MB per spike-2 run (0.12 MB per spike-1 run). A traced emulator run shows the program frees every exec allocation it makes, so this is probably filesystem caching of the large executable; the rebuilt package logs memory after every run and repeats one compile five times to tell a cache (levels off) from a leak (keeps falling).

**Done (2026-10-10): A1200 rerun of spike-2** (results in `tests/a1200/results/ogles2-spike-2`):
- **GPU code on real hardware:** `tex`, `light` and `skin` (725 QPU instructions) are identical to x86-64. `proc` differs only in two folded constants, by one ulp, the same way the 68020/68881 build did: Emu68 runs the 68040's single-precision FPU instructions in double precision (§8.4). The compiler module therefore moves to software floating point, which is identical to x86-64 on all four programs in the emulator and uses no FPU instruction.
- **Times** (68040 FPU build): start-up 1.04 s, one program including start-up 1.18 to 1.31 s, all four in one run 1.64 s.
- **Memory:** free fast memory drops 3.9 MB after the first run and then stays flat (within about 1 KB) over 15 more runs, including five identical repeats: a one-time cache, not a leak.

**Done (2026-10-10): A1200 run of the soft-float spike-2.** **All four programs' GPU code is identical to x86-64 on the A1200** (874 QPU instructions, uniform streams, shader-db statistics). Software floating point costs about 3%: all four programs in one run 1.69 s (1.64 s with the FPU), start-up 1.06 to 1.08 s, one program including start-up 1.19 to 1.34 s. Memory again a one-time drop, then flat. This closes D5 and the stream C spike.

**Done (2026-10-10): stream B, first part** (`v3dp/`, `dev/spike-c/harness/`):
- **`.v3dp` format proposed** (`v3dp/FORMAT.md`, §8.3), with the writer (`v3dp_writer.c`), an independent reader (`v3dp_dump.py`) and Mesa patch 0009 (variant hook, §8.2).
- **`v3dc` writes `.v3dp` files** (`-o <dir>`) and builds both for 68k and natively on the PC (`build_v3dc_host.sh`), which is the start of `v3dglslc` (§8.2).
- **The x86-64 and the 68k (emulator) builds write identical files**, byte for byte, for all four test programs (`compare_v3dp.sh`), and the reader decodes them without error.
- Findings: the link-time variants' keys differ from draw-time keys (`cbufs = 0xff` for `gl_FragColor`; all vertex outputs kept), so `v3dglslc` must draw per key state (§8.2); V3D stores uniforms with every vector and matrix column on 4 values; the `skin` program's indexed `bone[]` array is read through `QUNIFORM_UBO_ADDR` as §7.8 expected. The source hash covers the exact text, so line ends matter (the test scripts use LF, as the A1200 packages do).
- A1200 package ready: `tests/a1200/out/ogles2-v3dp-1.lha` (writes the four files on the A1200; check with `dev/spike-c/harness/check_a1200_v3dp.sh <results>`).

**Done (2026-10-10): A1200 run of `ogles2-v3dp-1`** (results in `tests/a1200/results/ogles2-v3dp-1`). **All four `.v3dp` files written on the A1200 are byte-identical to the x86-64 ones** (`check_a1200_v3dp.sh`). The run took 1.82 s for all four programs (1.69 s in the soft-float spike-2 run without writing; the cache bypass recompiles `tex.vert` for the fourth program). Free memory again dropped once, by 3.9 MB, as in spike-2.

**Done (2026-10-10): stream B, second part; `.v3dp` format frozen.**
- **Draw-time variants** (§8.2): `v3dc -o` draws triangles, lines and points on the fake device and stores what the driver compiles for them; the keys are now the ones real draws compute (`cbufs = 1`, vertex outputs limited to what the fragment shader reads, separate line and point fragment shaders, point-size vertex shaders where `gl_PointSize` is written).
- **A fifth test program** (`shaders/state.*`) uses every table: state variables (`gl_DepthRange`, the `gl_FragCoord` and `gl_PointCoord` y-transforms), 2D and cube samplers, a `mat2` attribute, struct-array, bool and int uniforms.
- **The x86-64 and 68k builds write identical files for all five programs**; the earlier QPU-listing comparison is still identical.
- **One more 68k bug fixed:** gallium's state-object cache asserted that state structs are a multiple of 4 bytes, which 2-byte alignment breaks (Mesa `patches/0010`, no effect on x86). It only showed up once the tool drew.
- **`FORMAT.md` frozen as version 1.0** (§8.3).
- A1200 package ready: `tests/a1200/out/ogles2-v3dp-2.lha` (five programs; check with `check_a1200_v3dp.sh`).

**Done (2026-10-10): A1200 run of `ogles2-v3dp-2`** (results in `tests/a1200/results/ogles2-v3dp-2`). **All five `.v3dp` files written on the A1200 are byte-identical to the x86-64 ones**, draw-time variants included (`check_a1200_v3dp.sh`). The run took 2.73 s for five programs. That is tool cost, not the runtime's: while capturing, every draw recompiles all three shaders (45 compiles for 27 stored variants), where the S2 runtime compiles one variant on a miss. Memory again dropped once by 3.9 MB. Stream B's format work is complete; `v3dglslc` moves into the repository in M0.

**Done (2026-10-10): M0, first part** (repository `this repository`, D7; licence D14):
- **Repository** with the stream B tools moved in (`v3dp/`, `tools/v3dglslc/`, renamed `v3dc` → `v3dglslc`). A from-scratch rebuild in the new layout (908 Mesa files for 68k, both builds) passes every check in 91 s, including the A1200's `.v3dp` files.
- **Generator** (`gen/gen_ogles2.py`, §6.6): 148 entries (the 142 GLES 2.0 commands from `gl.xml` plus 6 `agl*`), append-only `abi_order.txt`, dispatch struct, table, stubs for every entry not yet written, client wrappers, `aglGetProcAddress` thunks with a sorted name table, and an ABI layout test. Two traps found: OS macros on field names (above, §6.2), and clib2's `offsetof`, which is not a constant expression, so a typedef-array "static check" silently became a run-time array; the test now uses `_Static_assert` with `__builtin_offsetof`, and a deliberately wrong offset fails to compile.
- **Library skeleton**, written fresh (D14): resident tag, open/close/expunge, vector -30 (dispatch table) and a private -36 (log flush), the file log (RAM ring buffer, written by `OGLES2Close`, `aglDestroyContext` and the `OGLES2Log` command; not from the Close vector, which exec runs under `Forbid()`), minimal contexts, `glGetError`, `glGetString`. `libogles2.a` with `OGLES2Open/Close`, the current context and the `agl*` functions.
- **`libv3dcore.a`**: PiStorm3D's backend copied (`v3dcore/PROVENANCE.md`), built with PiStorm3D's flags; the mailbox state moved into `V3DDevice`, and `v3d_read_info` now fills `vpm_size`, `qpu_count`, `rev` and the clipper granularity as Mesa does. The library starts the GPU at the first context creation (`OGLES2_CCE_NO_GPU` without one) and reports it in `GL_RENDERER`. It does not call `v3d_free` at expunge, which would switch the QPUs off under a minigl.library in use (D3).
- **Tested in the emulator** (vamos loads the real library from disk): the smoke test passes; 20 open/create/destroy/close sessions lose no memory; the log file is written. vamos does not maintain exec's `TDNestCnt`, and has no GPU (the smoke test's `NOGPU` mode).
- **GPU submission lock, phase 1** (§7.1): `v3dcore/hw/v3d_gpu_lock.c`, the public `"V3D 4.2 submit"` semaphore (created by the first GPU client in public memory and never removed; obtained with `AttemptSemaphore` and 2 ms `timer.device` waits up to a timeout). The library creates it with the device; M1's job code holds it from binning submit to render completion. The library also logs a warning when `minigl.library` is open.
- **Post-build table check** (`gen/verify_table.py`, part of every build): reads the linked hunk file and checks that each of the 148 slots is relocated to the function of the same name; a deliberately swapped `abi_order.txt` fails with the two slots named.
- A1200 package ready: `tests/a1200/out/ogles2-m0-1.lha`.

**Done (2026-10-10): vbcc client test** (§6.3). vbcc 0.9h patch 2, vasm 1.9 and vlink 0.17 installed in `~/vbcc` (`dev/host-setup/build_vbcc.sh`, checksummed, reproduces the install byte for byte). `tests/abi_probe` passes all 30 checks built with GCC and with vbcc, and the smoke test passes built with vbcc. For it the library gained its first real GL state functions (`ogles2/state.c`: capabilities, write masks, clear values, line width, depth range, polygon offset, sample coverage, current vertex attributes, and the `glGet*` conversions of GLES 2.0 §6.1.2 for them), and a test-only tag, `OGLES2_CCT_PISTORM_NO_GPU_OK`, that creates a state-only context without a GPU so these tests also run in the emulator.

**Done (2026-10-10): A1200 run of `ogles2-m0-1`** (results in `tests/a1200/results/ogles2-m0-1`; the package without the vbcc probes). Every check passed, no crash:
- **The GPU is found and identified:** `V3D 4.2.14 (PiStorm), 8 QPUs, 64 KB VPM`, clipper granularity 256, exactly what Mesa's device-info code gives for the Pi 4. The new `v3d_read_info` reads the identity registers correctly.
- Smoke test passed twice (both sessions of 20 open/create/destroy/close cycles: 0 and +104 bytes, no leak); the library stayed loaded between the two runs (one `init` in the log); the log was written to the path from `ENV:OGLES2/LogFile`; `OGLES2Log` works; the ABI layout check passed; the submission lock exists and is free.
- Not proven by that run: that the library leaves memory at `Avail FLUSH` (free memory rose by 1.9 MB, but FLUSH also drops other caches). `smoke GONE` now checks exec's library list directly.

**Done (2026-10-10): A1200 run of `ogles2-m0-2`; M0 complete** (results in `tests/a1200/results/ogles2-m0-2`). All five steps passed, no crash:
- **Both compilers' programs work with the library on real hardware:** the ABI probe passes 30 of 30 built with GCC and with vbcc (byte and float arguments, `GLboolean` returns, varargs, thunks; the state functions run on Emu68's FPU), and the vbcc-built smoke test passes.
- **The library leaves memory:** after `Avail FLUSH`, `ogles2.library` is no longer in exec's library list.
- The GPU submission lock was found at the same address as in run 1, seven minutes earlier without a reboot: the semaphore outlives the library, as designed, and a later client finds it instead of creating a second one.
- Three smoke runs, 60 sessions: 0, 0 and +24 bytes; GPU identity as in run 1.
- **M0's exit criteria are met:** the library opens and closes on the A1200 with no leak; the ABI test passes for GCC and vbcc; the smoke test reports the V3D identity and device info.

**In progress: M1, step 1 of 3 (clear and present)** — built, waiting for the A1200 (`tests/a1200/out/ogles2-m1-1.lha`):
- **Surfaces** (`ogles2/display.c`, after PiStorm3D's `vid_*`): fullscreen (`MODEID`, our screen plus a backdrop window for input), the application's window (`WINDOW`, offscreen 32-bit bitmap plus `ClipBlit`), the application's bitmap (`BITMAP`), and a window ogles2 opens itself when none is given. The bitmap is locked only for one frame. BGRA32 and RGBA32 targets are accepted (the tile store's R/B swap covers both); anything else is refused with `OGLES2_CCE_NO_DISPLAY` and a log line.
- **Jobs v1** (`ogles2/job.c`, from `gl_FrameBegin`/`gl_FramePresent`): the GPU lock is taken before the bitmap lock and held until the render has finished (phase 1); nothing is logged while the bitmap is locked; the default framebuffer's depth is cleared by the hardware per tile and never loaded or stored (§7.2).
- `glClear` (whole framebuffer; scissored or masked clears are not confined yet and say so in the log), `glViewport`, `glScissor`, `glFlush`, `glFinish`, `aglSwapBuffers`, `aglSetParams2` (`VSYNC`, the output tags), queries for viewport, scissor box and channel bits.
- **New PiStorm tags:** `OGLES2_CCT_PISTORM_WIDTH`/`_HEIGHT` (size of ogles2's own window or screen), `OGLES2_CCT_PISTORM_GET_WINDOW` (the input window, which an application needs when ogles2 opened the window or screen), and `OGLES2_CCT_PISTORM_STATE_ONLY` (tests: a context without surface or GPU), replacing `_NO_GPU_OK`.
- **v3dcore:** the retire list moved into `V3DFrame` (`v3dcore/PROVENANCE.md`).
- **Not yet:** D24S8 (a stencil request gets D32F and 0 stencil bits for now), `aglSetBitmap` and retargeting through `aglSetParams2`, resize.
- **Test** `tests/m1_clear`: for each surface kind, six clear colours read back from what is displayed (window after the `ClipBlit`, screen, or bitmap), a frame without `glClear` must keep the picture (the colour-load path), 100 frames timed, and the context's memory returned at destroy. The package's library logs every line at once (`EXTRA_CFLAGS=-DOGLES2_LOG_SYNC`), so a hang loses nothing.

**Done (2026-10-10): A1200 run of `ogles2-m1-1`** (results in `tests/a1200/results/ogles2-m1-1` and `ogles2-m1-1.2`, five runs in all). **The GPU path works on real hardware:** every surface kind opened (application bitmap, ogles2's window, application window, fullscreen 640x480 BGRA), 16 contexts rendered 107 frames each with no dropped frame and no timeout, and every context returned its GPU memory (`0 bytes of GPU memory still in use`). 100 clear+swap frames took 0.2 ms each into a 256x128 bitmap, 0.4 ms fullscreen at 640x480, and 1.6 ms in a 320x240 window (the `ClipBlit` dominates).
- **Found:** every colour channel of 1.0 came out as 0, through the GCC 6 conversion bug above (§2.3), now worked around and checked by every build. The "frame without `glClear`" check failed as a consequence (its magenta was black too), so the colour-load path is still to be confirmed; the bitmap step's memory "leak" was the test measuring its own bitmap.
- Package `ogles2-m1-2` repeats the run with both fixed, and holds each colour 0.3 s so the surfaces can be seen.

**Done (2026-10-10): A1200 runs of `ogles2-m1-2` and `ogles2-m1-3`** (results in `tests/a1200/results/ogles2-m1-2` and `ogles2-m1-3`). Both still showed `ff` as `00` and `bf` as `c0`, and every surface kind ran without a dropped frame and returned its GPU memory. **Neither run used its own library:** their logs have no `init` line, run 3's log numbering continues from earlier runs (it starts at line 676), and run 3's step 6 never logged its probe. Run 1's library, the one with the GCC 6 `fintrz` bug, was still in memory (with 1.9 GB of fast RAM nothing forces an expunge), and `OpenLibrary` returned it; `Assign LIBS: ... ADD` only adds a directory to search when the library is not loaded. While chasing the fault we suspected the store's dither mode, then checked the packaged code, Emu68's `FINTRZ` (AArch64 `FRINTZ`, correct) and the clear packets against Mesa (correct); all of it was sound.
- Kept from those runs: the "keep the picture" frame now clears only depth (`glClear(0)` starts no GPU job); ogles2 does not dither into an RGBA8 framebuffer (Mesa never does; there is nothing finer to dither from), and probe `D` brings it back; the visible tests hold each state 1 s and the Shell says what to look for.
- New probe switch `ENV:OGLES2/Probe`, one letter per probe: `D` dithers the store, `C` logs each job's packed clear words.
- New test `m1_probe`, run on a hidden bitmap: (1) the CPU's float-to-byte conversion; (2) `ReadRGBPixel` against bytes the CPU wrote; (3) GPU load and store of a CPU-written pattern holding all 256 values in every channel (a frame that clears only depth); (4) GPU clears to all 256 values in every channel, read raw. Each prints which values changed, as `in>out`.

**Done (2026-10-10): A1200 run of `ogles2-m1-4`, the first remote run** (results in `tests/a1200/results/ogles2-m1-4-20261010-210139` and `-210328`), with the library loaded fresh (the A1200 had rebooted; the second run after `Avail FLUSH`). **Every check passes:** the CPU conversion, the readback, **GPU load and store of all 256 values in every channel unchanged** (so the colour-load path works), **GPU clears to all 256 values in every channel exact**, and `m1_clear` on the bitmap 10 of 10, including red, green, blue, white, `4080bf` and the depth-only frame keeping magenta. The colour fault was only ever the GCC 6 bug.
- Guard against a stale library: `runpkg` and the package scripts run `Avail FLUSH` first, and `runpkg` warns when the log has no `init` line (§10.1). Verified: `Avail FLUSH` removes an unused `ogles2.library` from memory.

**Done (2026-10-10): remote test station, step 1 of 4** (`tools/amictl`, §10.1):
- `amictld`: a portable core (`server/amictld.c`: protocol, token, allow list, log, job table) over an OS layer for AmigaOS (`os_amiga.c`: Roadshow's `bsdsocket.library`, jobs in their own launcher processes running a script with a 200 KB stack, `ReadPixelArray` screenshots, self-update keeping `amictld.old`) and for Linux (`os_posix.c`, for testing). 31 KB for the A1200, GCC 6 + libnix; libnix's stack swap gives it 64 KB, since `Run` from `S:User-Startup` would give only 4 KB.
- `amictl.py`: the client, also usable as a module; `runpkg` uploads a package, runs its script and fetches `results/`.
- **`make amictl-test`: 22 of 22 checks pass** against the host build: authentication and refusals, a 1 MB round trip with no temporary file left, directories, exit codes, Ctrl-C on timeout, asynchronous jobs, a screenshot written as PNG, `ENV:`, oversized and malformed requests, self-update, quit, and a client not on the allow list.
- Found while building: exec's `<exec/io.h>` already defines `CMD_START` and friends, and a system header includes a file named `proto.h`, so the protocol names carry an `AMC_` prefix and the header is `amictl_proto.h`. vamos cannot run the server (no TCP/IP stack, and its `CreateDir` does not resolve `PROGDIR:`), so its first real run is on the A1200.
- The installation package `ogles2-amictld-1` (token and allow list from `tools/amictl/station.local`, which stays out of git; the PC is `192.168.1.32`).

**Done (2026-10-10): remote test station, steps 2 and 3** (most of 3). The developer installed the server as `Stuff:amictld` and started it by hand. From this PC: `HELLO`, `INFO` (Kickstart 47.115, libraries, tasks), a 300 KB `PUT`/`GET` round trip byte-identical, `RUN` with output and return codes, `DELETE`, `GETENV`, `SHOT` (1920x1080 Workbench screen, correct), and `runpkg`. Found: `runpkg` assumed a `Work:` volume (fixed: it asks the server for its work directory); in a `RUN` command line `;` starts an AmigaDOS comment, so commands go on separate lines. Still to check: `REBOOT` and the automatic restart, which wait for the boot line (below).

**Done (2026-10-10): auto-start, remote reboot, and M1 step 1 on all four surfaces.**
- `S:User-Startup` now ends with `Run >NIL: Stuff:amictld/amictld` (written remotely, the original kept as `S:User-Startup.pre-amictld`). `REBOOT` flushes `PROGDIR:` and `SYS:` first (`ACTION_FLUSH`), then `ColdReboot`; the A1200 came back clean and the server answered 17 s after the reboot. Server 1.2 was installed through its own `RESTART` (self-update) twice.
- **`ogles2-m1-5`** (results in `tests/a1200/results/ogles2-m1-5-20261010-211759`), run remotely with a screenshot about every 2 s (`runpkg --shots`): **all 41 checks pass** on the application bitmap, ogles2's window, the application's window and fullscreen 640x480; every context returned its GPU memory, 159 frames each, none dropped. The screenshots show the colours on screen. **M1 step 1 is done.**
- In the first m1-5 run the memory checks failed by 8 MB: the server's screenshot buffer being allocated and freed while the test measured. The server now keeps its reply buffer for the connection.
- **Open: large replies sometimes stall.** A 1 to 6 MB `SHOT` reply stops partway and never resumes until the client gives up; 4 of 30 screenshots in a row with nothing else running, so it is the network path (server, Roadshow or `genet.device`), not ogles2. `SO_SNDTIMEO` of 30 s did not end the stall. Small transfers (packages, results, logs) have never stalled. Until it is understood, use screenshots sparingly; the client reconnects after a failed one.

**Done (2026-10-10): M1 step 2, first part: the first triangles.**
- **The built-in program's machine code is generated at build time**: `tools/builtin_shaders/mkshaders.c` runs PiStorm3D's QPU assembler under vamos (`make builtin-shaders`) and writes `ogles2/builtin_shaders.h` (checked in): `COORDINATE_TEXTURED` 57, `VERTEX_SMOOTH` 68 and `FRAGMENT_UNTEXTURED_SMOOTH` 18 instructions, little-endian. The library carries neither the assembler nor its 95 shader sources (about 195 KB of static buffers).
- **Program objects** (`ogles2/program.c`): shaders and programs in one per-context name table, sources kept (S1), deletion deferred while attached or current, `glBindAttribLocation`, attribute and uniform locations, `glGetActive*`, `glGetProgramiv`/`glGetShaderiv` and info logs, uniform storage, `glUniformMatrix4fv`/`glUniform4f(v)`/`glGetUniformfv`. M1 links every program to the built-in image (`a_position` vec3, `a_color` vec4, `u_mvp` mat4), logged once; M2 replaces that with the `.v3dp` lookup.
- **Program images** (`struct OGLES2Image`, `ogles2_internal.h`): code per stage, attribute and uniform tables, a uniform recipe per stage (`OGLES2_U_CONSTANT`, `UNIFORM`, the viewport and depth-range values), varyings and VPM segment sizes: the in-memory form a `.v3dp` loads into.
- **Draw path** (`ogles2/draw.c`): client-side arrays (float, fixed converted to float, byte, short; normalised or not; copied and byte-swapped per draw, tightly packed), disabled arrays from the current attribute, uniform streams from the recipes, the shader state and attribute records, and the state packets with per-control-list dedup in Mesa's order (clip window from viewport and scissor, CFG_BITS with culling, front face, depth function and writes, early-Z direction per frame as Mesa's `v3d_update_job_ez`, depth offset, POINT_SIZE every draw, line width, the viewport group, blend enables/config/constant colour, colour write masks, the per-pass residue, transform-feedback specs on a primitive change), `GL_SHADER_STATE` and `VERTEX_ARRAY_PRIMS`. New state: `glCullFace`, `glFrontFace`, `glDepthFunc`, `glBlendFunc(Separate)`, `glBlendEquation(Separate)`, `glBlendColor` and their queries. 77 of 148 entries are implemented.
- **A1200 run `ogles2-m1-6`, remote** (results in `tests/a1200/results/ogles2-m1-6-20261010-213956`): **all 57 `m1_draw` checks pass on the first hardware run**, on an application bitmap (D16), ogles2's window (D24, which gives D32F) and fullscreen 640x480 (D32F): smooth colours with red bottom left, green bottom right and blue top (the default framebuffer's Y flip is right), a constant colour from a disabled array, the matrix, back/front culling and `GL_CW`, the depth test both ways, viewport, scissor, blending (`80007f` for half red over blue), colour mask, strip and fan, a line and a point. 100 frames of 200 triangles take 0.4 ms each on the bitmap, 0.8 ms in the window and 0.7 ms fullscreen; every context returned all GPU memory, no frame was dropped, and `m1_clear` still passes.
- The culling winding follows PiStorm3D (`clockwise_primitives` set for `GL_CCW`), which compensates for the Y flip that its vertex shader does; with Mesa-compiled shaders (M2), the flip comes from a negative viewport Y scale (§7.11) and the winding has to be checked again.

**Done (2026-10-10): M1 step 2, rest: stencil, D24S8 and clears as rectangles. M1 step 2 is complete.**
- **Depth and stencil formats** (§7.2): a `STENCIL` request gives D24S8 (tile-buffer type `DEPTH24`, 24 depth and 8 stencil bits); otherwise up to 16 depth bits D16, more D32F. The stencil clear value goes into the ZS clear packet (its byte layout checked against Mesa's XML: PiStorm3D only ever sent 0).
- **Stencil:** `glStencilFunc/Op/Mask(Separate)`, `glClearStencil` and their queries; `STENCIL_CFG` packed byte by byte from Mesa's XML (§4.3; the hand-declared `v3d_stencil_cfg` bitfield struct is not used), one packet per face, deduplicated; CFG_BITS' stencil enable; early Z given up for the frame when the stencil test can reject or changes values on a depth failure (Mesa `v3dx_state.c`).
- **Clears the tile buffer cannot do** (§7.7) are drawn as a rectangle with the built-in program (`ogles2_draw_clear`): clears confined by the scissor box or by colour, depth or stencil write masks, and any clear after a draw in the same job (the tile buffer clears before every draw). The application's state is saved and restored around it. A clear of only depth or only stencil at a job's start still uses the tile buffer: neither is loaded, so they hold nothing to keep.
- **A1200 run `ogles2-m1-7`, remote** (results in `tests/a1200/results/ogles2-m1-7-20261010-214729`): **all 71 `m1_draw` checks pass** (23 on the D16 bitmap, 25 in the D24S8 window, 23 fullscreen D32F), with the new checks: a scissored colour clear, a colour-masked clear over a tile-buffer clear, a clear after a draw, a depth clear between two draws, the stencil test (mark with `GL_REPLACE`, then `GL_EQUAL` and `GL_NOTEQUAL`), and a scissored stencil clear. `m1_clear` 10 of 10; no frame dropped; all GPU memory returned.
- Known limit: a `glFlush` or `glFinish` in the middle of a frame ends the job, and the next job starts with depth and stencil cleared, since the default framebuffer never stores them (§7.2 plans storing them only at such splits; not done yet).

**Done (2026-10-10): M1 step 3, and with it M1.**
- **Hardware probes** (`tests/m1_hwprobe`; library switches `F`, `Y`, `S`, `A` in `ENV:OGLES2/Probe`, `ENV:OGLES2/ProbeAddr` for `A`): results above (§9, M1) and in §7.3, §7.5, §7.7 and §7.11. `Y` crashed the A1200 in run 8 (`tests/a1200/results/ogles2-m1-8-crash`: the probe output and the log up to the crash, read remotely after the reset, and a photo of the SmartCrash requester, a jump to `0x00000103`); runs 9 and 10 skip it unless asked (`m1_hwprobe Y`). `F` is now the normal path.
- **The device tree's memory ranges** are read at device init, every exec memory region is logged against them, and `v3d_mem_alloc` uses `MEMF_FAST` and refuses blocks outside them (`V3DDevice.gpu_reachable`, `v3dcore/PROVENANCE.md`).
- **Soak** (`tests/m1_soak`): 10,000 frames in ogles2's window with D24S8: a spinning smooth triangle and a ring of 50 small ones every frame, a scissored clear after the draws every 7th frame, the stencil test every 11th, the depth test every 13th, read-backs every 1000th. Runs 9 (`tests/a1200/results/ogles2-m1-9-20261010-220422`) and 10 (`tests/a1200/results/ogles2-m1-10-20261010-220544`): **10,000 frames in 9.0 s (0.8 ms each), 0 dropped, no GPU timeout, no GL error, every read-back right, all GPU memory returned** (`0 bytes of GPU memory still in use`), free memory back within 32 bytes.
- Run 10, after the allocation and `GL_FIXED` changes: `m1_draw` 71 of 71 on all three surfaces, the probes as in run 9, the soak as above.
- **M1's exit criteria are met:** smooth-shaded triangles in all three surface kinds, the 10,000-frame soak with no leak and no timeout, the probe results in this plan.

**Done (2026-10-10): M2 runtime, first part: compiled GLSL programs draw on the A1200.**
- **Program images** (`ogles2_internal.h`): compiled variants with their prog_data (threads, segment use, VPM sizes, VCM cache size, `vattr_sizes`, varyings and fragment flags), code and uniform recipe; attributes with input slots and columns; uniforms with storage places, strides and sampler indexes; per-stage constant buffers with their templates; state variables. The built-in image is described the same way.
- **The `.v3dp` reader** (`ogles2/v3dp.c`): every field read little-endian with bounds checks; refuses an unknown major version, V3D version or Mesa commit (only `74b69717c52b` is validated), a CRC mismatch, a recipe type or state variable the runtime does not implement, and spilling shaders; code copied once into GPU memory (freed after the open job when a program goes). Implemented recipes: `CONSTANT`, `UNIFORM`, the viewport X/Y scale and Z scale/offset, line widths, blend constants. State variables: `gl_DepthRange` (39), the `gl_FragCoord` Y transform (63) and the point-coordinate Y transform (64), computed per draw for the default framebuffer as Mesa does for a flipped one.
- **Linking** (`ogles2/program.c`): BLAKE3 of the sources (vertex first, `(stage, length)` little-endian before each text; `ogles2/blake3.c`, checked against Mesa's on 5,008 lengths by `make blake3-test`), then `PROGDIR:` and `OGLES2Cache:` (§7.8, 24-digit file names); without a binary the link fails and the sources go to `T:ogles2_missing_shaders.txt`. Attribute locations honour `glBindAttribLocation` (matrices take one per column); uniform locations number every array element (`name`, `name[i]`, struct members as stored); `glUniform{1,2,3,4}{f,i}{v}`, `glUniformMatrix{2,3,4}fv`, `glGetUniform{f,i}v`, `glGetActiveUniform`/`Attrib` for every GLES2 type, booleans as Mesa stores them (~0), samplers' units kept for the textures to come.
- **Draw path from prog_data** (`ogles2/draw.c`, Mesa's `emit_shader_state_record` and `v3d_emit_gl_shader_state` for V3D 4.2): the record's flags, threads and segment sizes from the variants, `v3d_compute_vpm_config` without a geometry shader (As 1, Ve 0, Vc from the shaders), `VCM_CACHE_SIZE` before every `GL_SHADER_STATE`, one attribute record per input slot with Mesa's GFXH-930/1602 rules, uniform streams with one word of prefetch slack, the clipper's Z scale/offset and min/max from the depth range, early Z given up when the fragment shader writes Z. Variants are chosen by primitive class (fragment) and per-vertex point size (vertex, binning); a miss uses the closest and is logged once.
- **Test binaries:** `make test-v3dp` writes the tests' GLSL (`tests/common/smooth_shaders.h`, and `light`, `skin`, `state`, `tex` from stream B) to files, compiles them with v3dglslc and names them by hash; packages ship them next to the tests.
- **`tests/m2_link`** (state-only context, runs under vamos too): 25 of 25 under vamos and on the A1200: `smooth` and `light` link from their binaries; locations, `mat3` columns 4 words apart, array elements, ints, count clipping and GL errors as the spec says; `skin` refused for `UBO_ADDR` (16), `state` and `tex` for `TMU_CONFIG_P0` (6); a program without a binary fails and is recorded.
- **A1200 run `ogles2-m2-1`** (`tests/a1200/results/ogles2-m2-1-20261010-222607`): `m2_link` 25 of 25; **`m1_draw` 71 of 71 with the Mesa-compiled program** on the bitmap, the window and fullscreen (colours, orientation, culling and `GL_CW`, depth, stencil, viewport, scissor, blending, masks, strips, fans, lines, points, clears); the 10,000-frame soak with it, 0 dropped, 0.9 ms per frame. The culling winding needs no change for compiled shaders (§7.11). 101 of 148 entries are implemented.

**Done (2026-10-10): M2 textures v1.**
- **`ogles2/texture.c`:** texture objects (generated names and any name, per-unit 2D and cube bindings, 16 units, the default objects, deletion unbinding), `glTexImage2D`/`glTexSubImage2D` with every GLES2 format and type converted to RGBA8 by GL's rules (`GL_ALPHA` samples (0,0,0,A), the 16-bit formats expanded from the 68k's own byte order) and the unpack alignment, `glTexParameter*`/`glGetTexParameter*`, `glPixelStorei`, `glGenerateMipmap` (CPU 2x2 box filter, plan 7.6), completeness (GLES 2.0 3.7.10) with the 1x1 (0,0,0,1) fallback for incomplete textures, and the texture queries (`GL_MAX_TEXTURE_SIZE` 4096, 16 units, 0 vertex units). Storage and tiling are PiStorm3D's `v3d_texture.c`; the texture and sampler state records are written by ogles2 (32-byte aligned, Mesa's LOD rules: clamped to 1/256 without mipmapping; `MIRRORED_REPEAT`).
- **The `TMU_CONFIG_P0/P1` recipes** (Mesa's `write_tmu_p0/p1`): each sampler uniform's texture index per stage reads the unit `glUniform1i` set; the state address carries the compiler's configuration in its low bits. `state` and `tex` now link.
- **v1 simplifications, each logged once:** a texture changed after a draw of the open job used it renders that job first (plan 7.7 hazard 3; renaming comes later); cube maps sampled as incomplete until M2's cube maps (below, §13); every format is stored as RGBA8 (the native 1- and 2-byte formats of the 7.6 table come later).
- **Found:** growing a mip chain parks the old level-0 block on v3dcore's global park list, which nothing drained (130 KB lost per context in the first run). ogles2 frees it at once (`v3d_texture_drain_parked_all`): nothing can still use it.
- **A1200 run `ogles2-m2-2`** (`tests/a1200/results/ogles2-m2-2-20261010-223835`), remote: **`m2_texture` 46 of 46** on a bitmap and in a window (orientation, the seven formats, the unpack alignment, nearest and linear filtering, the three wraps, uploaded mip levels, `glGenerateMipmap`, the incomplete fallback, two units, a texture replaced between two draws of one frame, deletion, `glTexImage2D` errors), `m2_link` 25 of 25, `m1_draw` 71 of 71; 0 bytes of GPU memory left after every context. 116 of 148 entries are implemented.

**Done (2026-10-10): M2 indexed draws, indirectly indexed uniform arrays, the program cache.**
- **`QUNIFORM_UBO_ADDR`** (unit 0, the stage's own constant buffer; other units are GLES3 uniform blocks and refused): a little-endian copy of the constant buffer per draw that needs it, made before any uniform stream, its address plus the recipe's offset. `skin` (a `mat4 bone[24]` indexed by a vertex attribute) now links and draws.
- **`glDrawElements`** with client indices, 8 and 16 bit (32 bit waits for `OES_element_index_uint`): the indices are scanned for their range, only those vertices are copied (Mesa's state tracker), the indices rebased and copied (16-bit ones to little-endian), then `INDEX_BUFFER_SETUP` and `INDEXED_PRIM_LIST` (no primitive restart in GLES2).
- **The program cache** (§7.8 step 1): every image a context loads stays in its cache, by hash, until the context goes; relinking a program, or another program with the same sources, reuses it.
- **A1200 run `ogles2-m2-3`** (`tests/a1200/results/ogles2-m2-3-20261010-224247`), remote: `m2_elements` 8 of 8 (both index sizes with a range that starts above 0, `skin` moved by `bone[5]` and `bone[20]` with the expected shade), `m2_link` 26 of 26 (the relink served from the cache: 10 loads for 11 links in the run), `m2_texture` and `m1_draw` on the bitmap 23 of 23 each; 0 bytes of GPU memory left. 117 of 148 entries are implemented.
- **With this, M2's runtime scope is in place** except two items: the corpus run, and buffer objects (needed by most real programs, though §9 lists them under M4; done next, below).

**Done (2026-10-10): buffer objects** (brought forward from M4, §7.5 "v1 as built").
- `glGenBuffers`, `glDeleteBuffers`, `glIsBuffer`, `glBindBuffer`, `glBufferData`, `glBufferSubData`, `glGetBufferParameteriv`; `glVertexAttribPointer` captures the `GL_ARRAY_BUFFER` binding (the pointer is then an offset); `glDrawElements` reads its indices from the `GL_ELEMENT_ARRAY_BUFFER`; the binding queries. Deleting a buffer unbinds it from the context's targets and attribute arrays (GLES 2.0 2.9).
- **A1200 run `ogles2-m2-4`** (`tests/a1200/results/ogles2-m2-4-20261010-225407`), remote: **`m2_buffers` 27 of 27** (object rules and errors, interleaved float and normalised-byte arrays drawn from vertex 4, `glBufferSubData` and `glBufferData` between two draws of one frame with the first draw keeping its data, 8- and 16-bit indices from an element buffer at offsets, a buffer attribute next to a client one, deletion while bound, reads past the end refused, 30 frames from one kept copy), `m2_elements` 8 of 8, `m2_link` 26 of 26, `m2_texture` and `m1_draw` 23 of 23 each; 0 bytes of GPU memory left after every context. **124 of 148 entries are implemented**; the 24 left are framebuffer objects (M5), `glReadPixels`, the copy and compressed texture calls, `glHint`, `glShaderBinary`, `glReleaseShaderCompiler`, `glGetShaderPrecisionFormat` and `aglSetBitmap`.

**Done (2026-10-10): the corpus runs** (M2 exit, except cube maps).
- **The harness** (`tests/corpus`): es2gears (Mesa demos) and ten OpenGL ES 2.0 Programming Guide samples compile **unmodified** against our versions of their frameworks, `eglut_corpus.c` and `esutil_corpus.c` (the book's `esShader.c`, `esShapes.c`, `esTransform.c` as they are). `corpus.c` runs `CORPUS_FRAMES` frames at a fixed 33 ms step (so both machines draw the same scene), captures frame `CORPUS_SHOT` with `glReadPixels`, and gives both sides one `rand()`. Backends: `corpus_amiga.c` (agl, its own window) and `corpus_host.c` (Mesa llvmpipe via surfaceless EGL into a framebuffer object). The host build also records each linked program's exact shader sources (in the format of `T:ogles2_missing_shaders.txt`); `v3dp/split_shaders.py` splits them and `v3dglslc` compiles them, so the A1200 needs one run. The sources stay outside the repository (`~/corpus-src`, cloned by `build_corpus.sh`); `compare.py` compares the captures (pass: under 2% of the pixels off by more than 40 of 255) and writes reference | capture | difference images.
- **Added for it:** `OES_element_index_uint` (the book's `esGenCube`/`esGenSphere` index with `GLuint`), `glReadPixels` (RGBA/UNSIGNED_BYTE, the implementation format too: renders the open job, reads the render bitmap bottom-up), `glHint`, `glGetShaderPrecisionFormat` (§7.8 values), `glReleaseShaderCompiler`, `glShaderBinary` (no formats: `GL_INVALID_ENUM`), `GL_SHADER_COMPILER` TRUE. `m2_elements` checks 32-bit indices and `glReadPixels` (13 checks).
- **A1200 run `ogles2-m2-5`** (`tests/a1200/results/ogles2-m2-5-20261010-231023`), remote, 11 programs in 22 s, no dropped frame, 0 bytes of GPU memory left after each: **10 of 11 match the PC**: es2gears (buffer objects, lighting), Hello_Triangle, Simple_VertexShader (32-bit indices), Simple_Texture2D, MipMap2D, TextureWrap, MultiTexture (two units, TGA textures), Stencil_Test, Multisample, ParticleSystem (point sprites, `gl_PointCoord`, additive blending): 0.00 to 0.03% of the pixels differ. **Simple_TextureCubemap** fails as expected (cube maps sample as incomplete).
- **Found on the way:** the clib2 maths trap (§2.3, `-lm881`); Stencil_Test sets `glStencilMask(0)` at the end of each frame, so its later frames read stencil values kept across `eglSwapBuffers`, which EGL leaves undefined (Mesa's framebuffer object keeps them, ogles2 does not): it is compared at frame 1. 129 of 148 entries are implemented; the 19 left are framebuffer objects (M5, 14), `glCopyTex(Sub)Image2D`, `glCompressedTex(Sub)Image2D` and `aglSetBitmap`.

**Done (2026-10-10): cube maps; the M2 exit is complete.**
- **Cube maps** (`texture.c`): each texture keeps six faces of levels (a 2D texture uses the first). On the GPU a cube map is six copies of the 2D level layout PiStorm3D's `v3d_texture_alloc`/`_mipchain` make, one after another at a face stride (Mesa's `cube_map_stride`, the tree's end aligned to 64 bytes; a 4 KB multiple here, so each face's level 0 keeps the alignment UIF XOR needs), uploaded face by face through a copy of the texture record whose base is moved by the stride. The texture state gets an image depth of 6 and the array stride; the compiled shader selects the face (its `TMUSCM` writes). Cube completeness (six square faces of one size, every face's chain consistent), `glTexSubImage2D` on faces, `glGenerateMipmap` on cube maps (`GL_INVALID_OPERATION` if not cube complete).
- **A1200 runs:** `ogles2-m2-5-20261010-231719`, **the corpus 11 of 11** (Simple_TextureCubemap 0.01% of the pixels off: the six faces where llvmpipe puts them); `ogles2-m2-4-20261010-231752`, regression: `m2_buffers` 27/27, `m2_elements` 13/13, `m2_link` 26/26, `m2_texture` and `m1_draw` 23/23 each; 0 bytes of GPU memory left everywhere.

**Next, in order (M3 is next by §9's order; these are M2 leftovers that can run alongside):**
1. **M3: the on-target GLSL compiler** (§8.4, stream C's design): `glCompileShader` compiling on the Amiga when no `.v3dp` is found.
2. **Native texture formats** (§7.6); texture renaming instead of the mid-frame flush.
3. **Depth and stencil kept across a mid-frame flush** (§7.2), and optionally across `aglSwapBuffers` for programs like Stencil_Test.
4. **Buffer copies:** re-swap only the range `glBufferSubData` changed (§7.5).
5. **More corpus:** SDL2's `testgles2` once an SDL2 build with the ogles2 backend exists.
6. **Large-reply stall** (`amictld`, when it gets in the way; the corpus's 0.9 MB captures came through).

---

## Appendix A: OpenGL ES 2.0 entry points (142, verified against Khronos `GLES2/gl2.h`)

Each group lists, where one exists, the PiStorm3D file to use as a template and the Mesa file to use as a reference. "New code" means neither has a counterpart.

**State and queries (13):** `glEnable glDisable glIsEnabled glGetError glGetString glGetBooleanv glGetFloatv glGetIntegerv glHint glFinish glFlush glPixelStorei glReadPixels`
- Template: `others.c`, `context.c` (`MGLReadbackBegin`).

**Rasterisation and per-fragment operations (26):** `glViewport glDepthRangef glScissor glCullFace glFrontFace glLineWidth glPolygonOffset glSampleCoverage glDepthFunc glDepthMask glClearDepthf glColorMask glClearColor glClear glClearStencil glStencilFunc glStencilFuncSeparate glStencilMask glStencilMaskSeparate glStencilOp glStencilOpSeparate glBlendColor glBlendEquation glBlendEquationSeparate glBlendFunc glBlendFuncSeparate`
- Template: `draw.c` `gl_EmitCullBlendState`, `context.c` `GLClear`.
- Reference: Mesa `v3dx_emit.c`.

**Buffer objects (7):** `glGenBuffers glDeleteBuffers glBindBuffer glBufferData glBufferSubData glIsBuffer glGetBufferParameteriv`
- New code (§7.5).

**Vertex input and drawing (16):** `glVertexAttribPointer glEnableVertexAttribArray glDisableVertexAttribArray glVertexAttrib1f glVertexAttrib1fv glVertexAttrib2f glVertexAttrib2fv glVertexAttrib3f glVertexAttrib3fv glVertexAttrib4f glVertexAttrib4fv glGetVertexAttribfv glGetVertexAttribiv glGetVertexAttribPointerv glDrawArrays glDrawElements`
- Template: `draw.c` `d_BuildShaderRecord` and primitive emission.
- Reference: Mesa `v3dx_draw.c`.

**Textures (18):** `glActiveTexture glGenTextures glDeleteTextures glBindTexture glIsTexture glTexImage2D glTexSubImage2D glCompressedTexImage2D glCompressedTexSubImage2D glCopyTexImage2D glCopyTexSubImage2D glTexParameterf glTexParameterfv glTexParameteri glTexParameteriv glGetTexParameterfv glGetTexParameteriv glGenerateMipmap`
- Template: `texture.c`, `v3d_texture.c`.
- Reference: Mesa `v3d_resource.c`, `v3dx_state.c`.

**Shaders and programs (29):** `glCreateShader glDeleteShader glShaderSource glCompileShader glShaderBinary glReleaseShaderCompiler glIsShader glGetShaderiv glGetShaderInfoLog glGetShaderSource glGetShaderPrecisionFormat glCreateProgram glDeleteProgram glAttachShader glDetachShader glLinkProgram glUseProgram glValidateProgram glIsProgram glGetProgramiv glGetProgramInfoLog glGetAttachedShaders glBindAttribLocation glGetAttribLocation glGetActiveAttrib glGetActiveUniform glGetUniformLocation glGetUniformfv glGetUniformiv`
- New code (§7.8, §8).
- Reference: Mesa `v3d_program.c`.

**Uniforms (19):** `glUniform1f glUniform1fv glUniform1i glUniform1iv glUniform2f glUniform2fv glUniform2i glUniform2iv glUniform3f glUniform3fv glUniform3i glUniform3iv glUniform4f glUniform4fv glUniform4i glUniform4iv glUniformMatrix2fv glUniformMatrix3fv glUniformMatrix4fv`
- New code.
- Reference: Mesa `v3d_uniforms.c`.

**Framebuffers and renderbuffers (14):** `glGenFramebuffers glDeleteFramebuffers glBindFramebuffer glIsFramebuffer glCheckFramebufferStatus glFramebufferTexture2D glFramebufferRenderbuffer glGetFramebufferAttachmentParameteriv glGenRenderbuffers glDeleteRenderbuffers glBindRenderbuffer glIsRenderbuffer glRenderbufferStorage glGetRenderbufferParameteriv`
- Template: `context.c` `gl_FramePresent` (tile-buffer load/store).
- Reference: Mesa `v3dx_rcl.c`, `v3d_job.c`.

---

## Appendix B: The AmigaOS 4 ogles2.library API surface and planned support

Source: `external/reference/glsnoop/src/ogles2_module.c` (public domain). glSnoop hooks every ogles2.library method and decodes every context tag. Usage examples: `external/reference/sdl-os4/src/video/amigaos4/SDL_os4opengles2.c`, `external/reference/gl4es/src/agl/agl.c`.

### B.1 Context functions
glSnoop hooks nine `agl*` interface methods:
- `aglCreateContext2`, `aglDestroyContext`, `aglGetProcAddress`, `aglMakeCurrent`, `aglSetBitmap`, `aglSetParams2`, `aglSwapBuffers`;
- the deprecated slots `aglCreateContext_AVOID` and `aglSetParams_AVOID`, which we provide as macros only.

SDL also calls the varargs forms `aglCreateContextTags2` and `aglSetParamsTags2`. glSnoop does not hook them, so on OS4 they are presumably SDK wrappers over the tag-list methods. We provide them the same way, as import-library functions that build the tag list and call the "2" methods.

**All are planned for M1** (§6.4).

### B.2 Context tags (`OGLES2_CCT_*`)
The semantics below are inferred from tag names and their use in SDL and gl4es. Confirming them against the OS4 autodocs (or with the library's author) is postponed (D9); the common tags (`WINDOW`, `DEPTH`, `STENCIL`, `VSYNC`) are already clear from SDL's code.

| Tag | Plan |
|---|---|
| `WINDOW` | M1: render to the window (offscreen bitmap + `ClipBlit`) |
| `BITMAP` | M1: host-owned bitmap |
| `MODEID` | M1: fullscreen screen in this mode |
| `CONTEXT_FOR_MODEID` | M1: same as `MODEID` (confirm) |
| `DEPTH`, `STENCIL` | M1: default-framebuffer depth/stencil bits |
| `VSYNC` | M1: `WaitTOF`; M6: proper swap interval |
| `GET_WIDTH`, `GET_HEIGHT` | M1: output tags returning the surface size |
| `RESIZE_VIEWPORT` | M1: reset the viewport on resize |
| `SINGLE_GET_ERROR_MODE` | M4: accepted; error-reporting mode |
| `SHADER_COMPAT_PATCH` | M2/M3: optional GLSL patching (missing `#version`/precision), as OS4 does |
| `DEBUG_SHADER_LOG` | M3: verbose compiler info logs |
| `DETECT_UNINITIALIZED_GLSL_VARS` | Accepted, ignored |
| `SPIRV_OPLINES`, `SPIRV_OPLINES_OFFSET`, `SPIRV_OPTIMIZE` | Accepted, ignored (no SPIR-V in this design) |
| `SHARE_WITH` | M6: shared object namespaces |

PiStorm-specific tags (for example MSAA, swap mode) go in a private value range.

### B.3 Extensions

| OS4 ogles2 exposes | Plan |
|---|---|
| `GL_OES_mapbuffer` (`glMapBufferOES`, `glUnmapBufferOES`, `glGetBufferPointervOES`) | M4 |
| `GL_OES_element_index_uint` | M4 |
| `GL_OES_get_program_binary` (`glProgramBinaryOES`, `glGetProgramBinaryOES`) | M2 (the S1 delivery mechanism) |
| `GL_OES_packed_depth_stencil` | M5 |
| `GL_EXT_texture_format_BGRA8888` | M4 |
| `GL_EXT_blend_minmax` | M4 (native blend modes) |
| `GL_OES_texture_float` | M6 (32-bit TMU return key) |
| `GL_EXT_texture_filter_anisotropic` | M6 |
| `GL_EXT_texture_lod_bias`, `GL_SGIS_texture_lod` | M6 (sampler state fields exist) |
| `GL_ARB_texture_rectangle` | M6 (Mesa `TEXRECT_SCALE` uniforms) |
| `glDrawElementsBaseVertexOES` (`OES_draw_elements_base_vertex`) | M4 |
| `glPolygonMode`, `glProvokingVertex`, `glGetBufferParameterivOES` (OS4 extras) | M6 (hardware supports polygon mode and provoking vertex through CFG_BITS; the query is trivial) |
| *Not in OS4 list, planned:* `OES_texture_npot`, `OES_compressed_ETC1_RGB8_texture`, `EXT_read_format_bgra`, `EXT_discard_framebuffer`, `OES_standard_derivatives` (Mesa's compiler supports derivatives; works in S1 and S2), `OES_depth_texture` | M4–M6 |

---

## Appendix C: Glossary
- **BCL / RCL:** binning control list / render control list. V3D renders in two passes: binning sorts primitives into tiles, then rendering processes tile by tile.
- **BO:** buffer object; here, any GPU-visible allocation.
- **CL:** control list, the command stream the V3D control-list executor reads.
- **Coordinate shader:** a position-only copy of the vertex shader that runs during binning.
- **drm-shim:** Mesa's fake kernel interface that lets the real driver and compiler run on a PC with no GPU.
- **prog_data:** the compiler's description of a compiled shader (inputs, VPM sizes, threads, flags) that the driver needs to build the shader-state record.
- **QPU:** the V3D shader processor (16-wide SIMD).
- **Recipe / uniform list:** the compiler's ordered list of what each word of a shader's uniform stream must contain.
- **TFU:** texture formatting unit, a V3D block for format conversion and mipmap generation.
- **TLB:** tile buffer, the on-chip colour/depth/stencil storage for the tile being rendered.
- **TMU:** texture memory unit (texture sampling and general memory loads).
- **UIF / UBLINEAR / LINEARTILE / raster:** V3D memory layouts for images.
- **VPM:** vertex pipe memory, which holds vertex shader inputs and outputs.
