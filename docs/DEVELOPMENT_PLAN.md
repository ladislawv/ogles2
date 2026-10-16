# ogles2.library for PiStorm (Raspberry Pi 4, V3D 4.2): development plan

| | |
|---|---|
| Date | Started 2026-10-09; status kept current in §13 |
| Status | In development: M0 to M5 done (real GLES2 programs, shaders compiled on the Amiga, the whole GLES2 core with framebuffer objects, faster than MiniGL); M6 (ecosystem, sharing, performance) next (§13) |
| Baseline | PiStorm3D / minigl.library 29.1 (`MiniGL_Library_68k`, branch `PiStorm3D` @ `01f5a3e`) |
| Target | AmigaOS 3.x, 68k under Emu68 on PiStorm with Raspberry Pi 4B / CM4 (the hardware PiStorm3D supports), RTG (Picasso96 or CyberGraphX) |
| Team | Ladislawv, R-Typer and Claude Opus 5.5 AI assistant (§2.4) |
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
- **`libogles2.a`**: the import library. It holds `OGLES2Open()`/`OGLES2Close()`, the client-side current context, and the `aglGetProcAddress` thunk table. `OGLES2Open()` opens the installed library (in memory, or `LIBS:`), else `PROGDIR:libs/ogles2.library`, else `PROGDIR:ogles2.library` (2026-10-15), so a program can bring the library in its own drawer; one too old or of another dispatch ABI is closed and the next tried. exec keeps one library of a name, and an `OpenLibrary` of any path returns the one in memory (tested on the A1200: `ogles2.library` and `PROGDIR:libs/ogles2.library` gave the same base in either order), so `OGLES2Open()` expunges (`RemLibrary`, as `Avail FLUSH`) one that would not do before the next try, if no program has it open; one in use cannot be replaced and the open fails. The order stays the system's first: a program's older copy loaded first would serve every program in the session.
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
| `aglSetBitmap(bitmap)` | Retarget to a host bitmap (`vid_AdoptBitMap` rules: 4 bytes per pixel, CGX-lockable). As built (M4): only for a context created with `OGLES2_CCT_BITMAP`; the open frame is rendered into the old bitmap first; a new size resizes the pools (`v3d_context_resize`), the viewport and scissor stay as the application set them; a bitmap that fails the checks is refused and the old one stays |
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
  - **As built (M6, first step):** overlap within one program while it is the only ogles2 context in the system (`OGLES2Library.contexts`): a flush may return once the render is submitted, keeping the lock (and the bitmap lock on the screen) until `ogles2_job_finish` at the next flush or wherever the CPU must see or change what the render uses. With a second context every flush waits as in phase 1, because the lock is polled every 2 ms and a holder that re-takes it at once would starve the other program into its 5 s timeout.
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
  - **As built (M6, first step):** fullscreen swaps return as soon as the render is submitted (the screen shows what the GPU writes, as MiniGL); a framebuffer object's job and a mid-frame flush do too. An application bitmap and the window mode's copy need the finished frame at the swap, so those swaps still wait. Window mode with a second offscreen bitmap was weighed and left out: the copy (`ClipBlit`, about 1 ms at 640x480) needs a finished frame, so it would show each frame one swap late, and a program that draws only when something changes would leave its last frame unshown.
- **Vsync** (`OGLES2_CCT_VSYNC`): `WaitTOF` before presenting, or `ChangeScreenBuffer`'s safe message once double buffering works.
  - **As built (M6, after the presentation investigation):** the swap waits for the display's refresh, then the frame goes out. On a screen of its own the wait is `WaitBOVP` (the screen's own refresh) and the frame's render is submitted right after it: the GPU writes the screen from the top faster than the display scans it out (a 640x480 frame in 1-4 ms against 16.7), so nothing tears, and the CPU builds the next frame while the GPU renders this one. In a window the wait is `WaitTOF` and the copy (`ClipBlit`) follows it. The task runs at priority 10 at least from the wait to the submission (woken late at priority 0 behind a busy task, about 2 refreshes a second were missed).
  - **Findings (A1200, `tests/m6_flip` and `m6_vsync`, package `m6-9`, the developer watching):** drawn straight into the visible bitmap without waiting, the picture tears. `ChangeScreenBuffer` and `ScrollVPort` take a whole refresh per call, at times busy-waiting it (a priority-20 process flipping in a loop froze the A1200; Quake III with two or three screen buffers ran at 60 fps falling to 30, 47-49 in the timedemo; later runs flipped only every second refresh, 30 fps). `WaitTOF` and `WaitBOVP` follow the HDMI refresh (16.7 ms) and sleep (a priority -1 counter kept 96% of an idle system's rounds). Screen buffers were therefore dropped.
  - **Result:** `m6_vsync` 60 fps with vsync, no tearing and no stutter (the developer watching); Quake III's demo played at its own speed with `r_swapInterval 1`: 16.8-17.2 ms a frame (60 fps), the render overlapped (2 µs waited for it), against 19-24 ms with screen buffers. SDL3 `testgles2 --vsync` (package `m6-4`, steps 7 and 8; SDL's full screen is a window on the Workbench screen, so both take the window path): 59.8 fps in a window, 60.1 full screen (16 761 and 16 666 µs between swaps), no tearing and no stutter (the developer watching).
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
- **v1 as built (`buffer.c`, M2):** one GPU copy per attribute layout (offset, size, type, stride, normalised), of the whole buffer from that offset, packed at a 4-byte stride as client arrays are; up to 8 layouts per buffer, kept until the data change. `glBufferData` drops every copy; `glBufferSubData` (since M4) re-converts in place only the elements the changed bytes touch, in each copy the open job does not read; a copy the open job reads is retired with that job (`v3d_frame_defer_free`), so a change between two draws of one frame neither waits nor flushes. Element buffers are read from the shadow and copied per draw, as client indices are. A draw that would read past a buffer's end is not drawn (logged).
- **`OES_mapbuffer` (as built, M4):** `glMapBufferOES` returns the shadow and `glUnmapBufferOES` marks it fully dirty (every copy starts afresh); `glGetBufferPointervOES`, `GL_BUFFER_ACCESS_OES` and `GL_BUFFER_MAPPED_OES` answer; `glBufferData` ends a mapping.
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
- **`glGenerateMipmap`:** v1 uses a CPU box filter per level plus upload. Since M6, the TFU (the GPU's texture formatting unit; Mesa `v3dx_tfu.c`, `v3dcore/hw/v3d_tfu.c`) filters levels 1..n in place from level 0 in GPU memory, for a 2D texture whose GPU copy already has the chain (the first call takes the CPU path, which lays it out) in a format the TFU filters (8-bit and 16-bit float ones). The levels it writes are newer on the GPU than their RAM copies, like a render target's (read back only when the CPU needs them). Cube maps, 32-bit floats, depth, and probe `M` keep the CPU path.
- **Uploads (M6):** the conversion into the RAM copy and the packing into 16-bit formats run one loop per format, the 16-bit types through two 256-entry tables (a switch per texel cost 50 ms a megapixel). A whole level of 512x512 and up in RGBA8, or of 128x128 and up in another format the TFU takes, is tiled by the TFU from its raster copy (`v3d_tfu_store_raster`, Mesa's buffer-to-image copy) when that copy lies in memory the GPU reaches; the CPU tiles the rest (RGBA8 directly into UIF at 5.6 ms a megapixel; the TFU's job costs 0.3-0.5 ms, then 1.9 ms a megapixel). Probe `U` keeps the CPU's tiling.
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
  - **As built (M6):** `OGLES2_CCT_PISTORM_MSAA` 4 (or `ENV:OGLES2/MSAA` 4 without the tag) gives the default framebuffer 4× MSAA; framebuffer objects stay single-sampled. Its jobs bin and render with `multisample_mode_4x` in 32×32 tiles (Mesa's `v3d_choose_tile_size` for one 32-bpp target; the context's tile pools are sized for them), the rasterizer oversamples (`CFG_BITS`), the colour is stored resolved (`DECIMATE_MODE_4X`) and loaded into every sample, and depth kept across a mid-frame flush is stored and loaded with all four samples (twice the width and height). `glSampleCoverage` sets `SAMPLE_STATE`'s mask (round(value×4) samples, inverted on request). The shaders need no variant: under GLES2 the fragment key's `msaa` changes no code (Mesa's compiler uses it for per-sample shading, interpolation at a sample, framebuffer fetch and software blending). Alpha to coverage (2026-10-15): the fragment shader turns alpha into the sample mask (Mesa's key `msaa` and `sample_alpha_to_coverage`), from the program's `.v3da` variants (Mesa patch 0014, `v3dp/FORMAT.md`).
- **As built (M5, `fbo.c`, `job.c`):**
  - jobs v1 as above: a job belongs to one framebuffer (`ctx->job_fb`); drawing into another renders it first, lazily (binding alone costs nothing); the default framebuffer's depth and stencil are kept across such a switch as at any mid-frame flush (§7.2);
  - an object's job follows Mesa's `v3dx_rcl.c`: colour loaded unless cleared and always stored, in the image's own layout (texture levels as `v3d_texture.c` laid them out, cube faces at their face stride, the `Clear Colors Part3` UIF pad when the image is 15 or more UIF blocks taller than the job); depth and stencil renderbuffers in UIF without XOR, loaded and stored every job (`EXT_discard_framebuffer` drops either); tile pools of its own only when it has more tiles than the default framebuffer;
  - colour renderbuffers (RGBA4, RGB5_A1, RGB565, and RGB8/RGBA8 from `OES_rgb8_rgba8`) are textures of their own, so readback and formats are the textures'; the 16-bit stores are `ABGR4444`, `ABGR1555`, `BGR565` without R/B swap, as Mesa's format table;
  - a level the GPU drew into is newer than its RAM copy: it is read back (`v3d_texture_load`) before anything works from that copy (`glTexSubImage2D`, `glGenerateMipmap`, renaming, a rebuilt chain); a texture changed while the open job draws into it renders that job first;
  - no dithering of 16-bit targets (Mesa sets no store dither either); deleting a texture or renderbuffer detaches it from every framebuffer object of the context (GLES2 asks this of the bound one only; the others would keep an image whose memory is gone).

### 7.8 Programs and uniforms
- **Shader object:** type, source, compile status, info log.
  - **S1:** `glCompileShader` only records the source and reports `COMPILE_STATUS` TRUE. The real work happens at link, against the cache (below). This is a documented deviation until S2.
  - **`GL_SHADER_COMPILER` must report TRUE in S1 as well.** The GLES2 spec requires `glShaderSource`, `glCompileShader`, `glGetShaderPrecisionFormat` and `glReleaseShaderCompiler` to fail with `GL_INVALID_OPERATION` when it is FALSE, which would disable the source-cache path. A build option for a strict binary-only mode (FALSE, binaries only) can come later.
  - **S2 (with `LIBS:v3dglslc`, M3):** `glCompileShader` compiles the shader in the compiler's front end (request `V3DGLSL_COMPILE`), for a real `GL_COMPILE_STATUS` and info log. A shader the compiler accepted gets a **record** in the disk cache, `<24 hex digits>.v3ds` (BLAKE3 over its stage and source, as for a one-shader program), holding its info log; the next `glCompileShader` of the same source reads the record and needs no compiler. A program that links from a binary records both its shaders too, so programs whose binaries are cached or shipped do not start the compiler again. A link with a shader that failed to compile fails, naming the shader, with its messages.
- **Program object:** attached shaders, attribute bindings, link status, info log, and after linking:
  - the loaded `.v3dp` image (§8.3), with shader code uploaded once into a code BO;
  - the **uniform table** (name → location, GL type, array size, storage offset) and **attribute table** (name → location → shader input slot);
  - **sampler bindings** (sampler uniform → texture unit);
  - **uniform storage**: host-order 32-bit words plus a dirty serial.
- **`glLinkProgram`** computes the program hash from the vertex and fragment sources (`v3dp/FORMAT.md`, section `PROGRAM`), then looks it up in order. Attribute bindings are not part of the hash: the runtime maps attributes to the shader's input slots by name, so one binary serves any `glBindAttribLocation`.
  1. in-memory cache;
  2. disk cache in a directory named by an assign (`OGLES2Cache:`), checking `PROGDIR:` first so applications can ship their own binaries. The file name is the hash's first 24 hex digits plus `.v3dp` (29 characters: FFS file names are at most 30); the full hash inside the file must match;
  3. the S2 compiler, if installed (`LIBS:v3dglslc`, §8.4): it writes the binary into the disk cache, and the library loads it from there;
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

  A `.v3dp` holds one or more variants. At draw time the driver computes the runtime key and picks the matching variant; on a miss it logs the miss and uses the closest variant, so that draw may render wrongly; for example, a triangle variant used for points leaves the point size undefined. The miss log names the key so the variant can be added.
  - **Checked against Mesa's key code (`v3d_program.c`, 2026-10-11): with the features implemented now, every GLES2 draw on the default framebuffer matches a variant both compilers always build** (triangles, lines and points). The fragment key's other fields are fixed there: `cbufs` and the colour formats change only with framebuffer objects, `msaa` and the sample fields only with MSAA, `logicop`, `line_smoothing` and `ucp_enables` do not exist in GLES2, and `sampler_is_32b` needs float textures. The one field that does vary, `can_earlyz_with_discard` (set when a shader with `discard` draws without depth or stencil writes), only enables early depth tests: the variant both compilers build (with depth writes on) leaves it off, which is correct in every state and at worst slower. **So compiling variants at draw time becomes necessary with framebuffer objects (M5), MSAA and float textures**, and is done with them: the request then carries the key, and the server adds the variant to the cache file.

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
| `MAX_VERTEX_TEXTURE_IMAGE_UNITS` | 16 (0 until M7; the draws bind both stages' samplers) | min 0 |
| `MAX_TEXTURE_SIZE`, `MAX_CUBE_MAP_TEXTURE_SIZE`, `MAX_RENDERBUFFER_SIZE` | 4096 | `V3D_MAX_IMAGE_DIMENSION`, `V3D_MAX_FRAMEBUFFER_SIZE(42)` |
| `MAX_VIEWPORT_DIMS` | 4096 × 4096 | |
| `ALIASED_POINT_SIZE_RANGE` | 1–512 | `V3D_MAX_POINT_SIZE` |
| `ALIASED_LINE_WIDTH_RANGE` | 1–32 | `V3D_MAX_LINE_WIDTH` |
| `SAMPLE_BUFFERS` / `SAMPLES` | 0 / 0; 1 / 4 with `OGLES2_CCT_PISTORM_MSAA` | `V3D_MAX_SAMPLES` 4 |
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
- **Packaging (decided 2026-10-11, M3): a resident server process, not a library.** The compiler is `v3dglslc` itself, installed as `LIBS:v3dglslc` (or, not installed, beside the program that links: `PROGDIR:libs/v3dglslc`, then `PROGDIR:v3dglslc`, 2026-10-15; the one found is kept as an absolute path, since another program's `PROGDIR:` is another drawer) and started as `v3dglslc SERVER`: it creates the V3D screen on the fake device and the surfaceless GLES2 context once, adds the public port `v3dglsl compiler`, and serves requests (`v3dp/v3dglsl_port.h`: both sources and the path of the `.v3dp` to write; the reply carries the result and the compile and link messages). `ogles2.library` (`ogles2/compiler.c`) starts it with `SystemTagList` (asynchronous, 4 MB stack, `NIL:` for input and output) on the first program it has no binary for, sends each such program, waits for the reply (giving up if the port disappears), and loads the written file like any other. Expunging the library, or `glReleaseShaderCompiler`, sends `QUIT` without waiting (a request without a reply port is the server's to free, which is what makes it safe under `Forbid()` in `LibExpunge`).
  - **Why a process instead of the library planned earlier:** the compiler is Mesa built with GCC 16 and libnix, `ogles2.library` is GCC 6 and clib2. As a separate program each keeps its own C runtime, Mesa's C++ static constructors run at program start as libnix expects, nothing of the spike's runtime list (call table for `malloc`, constructors at open, process-context functions) has to be built, and the binary is exactly the one already verified bit-identical to x86-64. Several programs compiling at once simply queue at the port. The cost of the separate start (about a second) is paid once per session.
  - **Where binaries go:** `OGLES2Cache:` if that assign exists (kept across reboots), else `T:` (until the next reboot). The library checks for the assign in DOS's list instead of opening it, so a missing assign never puts up an "insert volume" requester.
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
- **Closed (2026-10-11):** the last three criteria were met (§13); the first counted the 14 framebuffer-object entry points, which M5's scope holds, so M4 closed with them moved to M5 (recommended, and the work continued on it). M5 then implemented them: all 142 core entry points exist.

### M5: Framebuffer objects (M–L)
- **Scope:**
  - Framebuffer and renderbuffer objects with completeness rules; texture-level and cube-face attachments.
  - All renderbuffer formats including `OES_packed_depth_stencil`.
  - Multi-job frames, per-FBO tile pools, hazard flushing, `EXT_discard_framebuffer`, `glReadPixels` from FBOs.
- **Exit criteria:** render-to-texture, post-processing chains and depth-renderbuffer scenes match reference images; the §10 orientation tests pass on both target kinds; no leaks when FBOs are created and destroyed every frame.
- **Done (2026-10-11):** every criterion met on the A1200 (§13).

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
  - Release: SDK (headers, import library, examples, autodoc), LHA package, documentation of deviations (unsupported tags, any remaining limits). Done 2026-10-15 (`release/`, §13).
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

### 10.2 Frame-time targets

M6's exit criterion. Each row is a workload measured on the test A1200 (Pi 4B, Emu68, 640x480 or the size given, 32-bit RTG screen) and the bound a later build must stay within: about 10% above the measured time, so a regression shows and run-to-run noise does not. A change that crosses a bound is either fixed or the bound is moved here with the reason. The goals at the end are not met yet.

| Workload | How it is measured | Measured (2026-10-13) | Target |
|---|---|---|---|
| Quake III, renderergl2, timedemo `four`, 640x480 | `m6-5` `Run-M1` | 12.4 ms a frame average (80.5-81 fps); min 5.0, max 82 ms | at most 13.5 ms average (74 fps) |
| the same, 1920x1080, lighting maps off (the patch's default) | `m6-5` `Play-Quake WIDTH=1920 HEIGHT=1080`, `timedemo 1` and `demo four` in the console | 22.3 ms (44.8 fps); the GPU 9.7 ms | at most 25 ms (40 fps) |
| the same, 640x480, with vsync, the demo at its own speed | `m6-5` `Run-M1 VSYNC=1 TIMEDEMO=0` | 16.7 ms (60 fps steady; 16.8-17.2 ms per frame) | every frame one refresh, no 30 fps stretches |
| the same with 4x MSAA | `m6-5` `Run-M1 MSAA=1` | 81 fps, as without | no slower than without MSAA by more than 5% |
| Quake III on gl4es, 640x480, timedemo `four` | `m6-2` `Run-M1` | 23.3 ms (43.2 fps) | at most 25.5 ms |
| glmark2-es2, 800x600 window, default list, 10 s a scene | `m6-7` `Run-M1` | score 251-253; `refract` 48 fps | score 230 or more; 0 frames dropped |
| SDL3 `testgles2`, a window / full screen | `m6-4` steps 1 and 6 | 1.96 / 2.21 ms (504-511 / 445-452 fps) | at most 2.2 / 2.45 ms |
| the same with `--vsync` | `m6-4` steps 7 and 8 | 16.76 / 16.67 ms between swaps | at most 17.0 ms (one refresh missed in 60) |
| CPU time of one draw call (renderergl2's draws) | `m6-5` `Run-M1 PROFILE=1` | 28-32 µs | at most 35 µs |
| a swap's flush (render list, CPU cache before and after) | `ogles2.log` timing lines | 57 + 77 + 56 µs (a 640x480 SDL frame) | at most 210 µs together |
| `glTexImage2D`, 1024x1024 RGBA8 / RGB565 | `m6-8` (`m6_tfu`'s upload timings) | 9.2 / 24 ms | at most 10 / 27 ms |
| draw into a 512x512 target and `glGenerateMipmap` | `m6-8` | 1.18 ms | at most 1.3 ms |
| compile a typical program on the A1200, after the first | `m2-5` | under 1 s (the first 1.6 s) | under 1 s |
| Quake III's shipped shader cache loaded, 640x480 and 1080p | `m6-5` | 2.35 s, nothing compiled | at most 3 s |

**Goals not met yet:**
- Quake III on gl4es as fast as on MiniGL: 16.4 ms a frame (60.8 fps) against 23.3 ms. The gap is ogles2's CPU time per draw for gl4es's draw pattern (postponed, §13).
- glmark2's `buffer` scenes (49-83 fps) above 100 fps: half of a 200-column buffer rewritten every frame.

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
| D6 | Licence for new code | **Decided (2026-10-15)** | The library as built stays under the Hyperion terms (D14): it incorporates PiStorm3D's backend. Under them: `v3dcore/`, the six front-end files that adapt PiStorm3D's code (`job.c`, `display.c`, `draw.c`, `program.c`, `texture.c`, `builtin_shaders.h`) and `tools/builtin_shaders/`. Everything else written for the project is MIT, the import library and public headers (`client/`, `include/libraries/`, `include/clib/`) included: they hold no MiniGL code, so GPL programs (Quake III, glmark2) may link them, and the library is opened at run time. The ports keep their programs' licences (`LICENSE` section 4). The release needs no one's agreement: it meets the Hyperion terms (AmigaOS only; the source published, so the repositories go public with it). The backend's authors (Dennis van der Boon, Steffen Haeuser) will be asked later, once the release is otherwise ready, whether their code may become MIT; then the whole library would be |
| D7 | Repository | **Decided (2026-10-10)** | New local git repository `this repository` (not published yet): `v3dp/`, `tools/v3dglslc/`, `v3dcore/`, `ogles2/`, `client/`, `include/`, `gen/`, `tests/`, `dev/host-setup/`. This workspace keeps the plan, the release archive, the references and the spike record |
| D8 | Backend pluggability | Open | Keep `backendFlags` in the table, as minigl does |
| D9 | `OGLES2_CCT_SINGLE_GET_ERROR_MODE` and other OS4-specific tags | **Postponed** | Accept every tag; implement what is meaningful (Appendix B). Exact OS4 semantics are not needed before M1 |
| D10 | Default-framebuffer depth/stencil after `aglSwapBuffers` | Open | Undefined, as EGL specifies, so nothing is stored at swap; a PiStorm-specific tag opts into preserving them (§7.2) |
| D11 | Priority among GLSL, FBOs and SDL | **Decided: GLSL first** | Runtime GLSL compilation on the Amiga is M3, ahead of GLES2 completeness (M4) and FBOs (M5) |
| D12 | Team | **Decided** | One developer plus the AI assistant (§2.4) |
| D13 | Test and reference machines | **Decided** | A1200 with PiStorm for all hardware runs; this PC for builds, stream B and llvmpipe reference images (§10) |
| D15 | Remote test station | **Decided (2026-10-10)** | A server on the A1200 (`amictld`, Roadshow on `genet.device`, `192.168.1.128`, started at boot) and a PC client (`amictl`) upload, run and collect tests and screenshots over the LAN (§10.1). Token plus address check; every command logged on the Amiga |
| D14 | Licence | **Decided (2026-10-10): accept the Hyperion terms** | PiStorm3D's backend is under the Hyperion MiniGL Open Source License (its `Licence.txt`; only parts are MIT: Macoy Madson's and the Mesa-derived code). `libv3dcore` copies it, so `ogles2.library` as a whole is distributed under the Hyperion licence: AmigaOS only, and the source of all modifications published to the Amiga developer community. Third-party parts keep their own licences and notices. The library glue is written fresh (not copied from minigl's `libraryroot`), and stand-alone parts that incorporate no MiniGL code (`v3dp/`, `tools/v3dglslc/`, the generator) are MIT. Refined by D6 (2026-10-15): which files are under each |

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
- **As built (M4, 2026-10-11):** textures are stored in the hardware format their level 0's format and type map to (Mesa's `v3dx_format_table.c`: `RGB565`, `RGBA4`, `RGB5_A1`, `R8` with swizzle `XXX1` for LUMINANCE and `000X` for ALPHA, `RG8` with `XXXY` for LUMINANCE_ALPHA, else `RGBA8`), packed little-endian from per-level RGBA8 copies kept in RAM (for `glGenerateMipmap` and rebuilds); uploads use Mesa's convention for the tiled layout (the level's padded stride and padded height), which non-power-of-two sizes need. ETC1 is decoded to RGBA8 for now (native ETC1 sampling: later). A texture changed after a draw of the open job used it is **renamed** (the job keeps the old GPU copy until it has rendered; the texture gets a new one), and only a full retire list falls back to rendering the job first.
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

**Done (2026-10-11): M3, first part: programs are compiled on the Amiga.**
- The compiler server (§8.4 "Packaging"): `v3dglslc SERVER` and `ogles2/compiler.c`; `glLinkProgram` falls back to it after `PROGDIR:` and the disk cache, and puts its messages into the program's info log. `compile_common.c` now compiles from memory, collects the messages and deletes its GL objects after each program (the offline compiler's output is unchanged: the corpus binaries are byte-identical before and after).
- **A1200 run `ogles2-m3-1`** (`tests/a1200/results/ogles2-m3-1-20261011-122828`), remote, a package **without any `.v3dp` file**: `m2_link` 26/26 (with two new checks: a program without a binary links, compiled; a GLSL error fails the link with the compiler's message in the info log), `m2_elements` 13/13, `m2_texture` 23/23, `m1_draw` 23/23, **the corpus 11/11**; 17 programs compiled. **All 16 that have an offline twin are byte-identical to `v3dglslc` on the PC** (the 17th is the test's deliberately unique program). The server started in under a second; the first compile took 1.6 s, the others 0.1 to 0.64 s; the whole run 31 s. After the library's expunge the server was gone from `Status`. 0 bytes of GPU memory left after every context.
- **M3 exit criteria now:** corpus output identical to `v3dglslc` (met); compile time under 1 s per typical program after the first (met); programs work without a pre-filled cache (met); real compile and link info logs (met by the next step).

**Done (2026-10-11): M3, second part: compile status at `glCompileShader`, shader records** (§7.8 "S2").
- A third request, `V3DGLSL_COMPILE` (one stage, front end only); `.v3ds` shader records in the disk cache; a link that fails because of a shader names it and carries its messages.
- **A1200 run `ogles2-m3-2`** (`tests/a1200/results/ogles2-m3-2-20261011-123438`), remote, without `.v3dp` files: `m2_link` 28/28 (new: a broken shader reports `GL_COMPILE_STATUS` false at `glCompileShader`, with Mesa's message `0:2(33): error: 'undefined_name' undeclared` in its info log; a correct one compiles), `m2_elements` 13/13, `m2_texture` and `m1_draw` 23/23 each, the corpus 11/11; 17 programs and 30 shaders compiled, the 16 programs with an offline twin byte-identical to it. **Then the warm cache:** after a flush (which also stopped the compiler), `m2_link` again 28/28 and es2gears again matching its reference, with no compile at all: the only requests were the test's deliberately broken shader (failures are not recorded). Run 36 s.
- **Without the compiler nothing changed:** `ogles2-m2-4` (`tests/a1200/results/ogles2-m2-4-20261011-123529`) all 5 tests pass, `m2_link` 26/26 on the missing-binary path.

**Done (2026-10-11): M3 complete.**
- **Draw-time variants:** not needed for correctness until framebuffer objects, MSAA or float textures exist (§7.8, checked against Mesa's key code); moved to those features (M4/M5).
- **`OGLES2_CCT_DEBUG_SHADER_LOG`:** a context with it gets link info logs that describe what the program runs, one line per variant (stage, primitive class, instructions, threads, spill bytes, uniforms), also in the library's log. It works for binaries from the cache and from `PROGDIR:` alike, since the numbers come from the `.v3dp`.
- **A1200 runs** `ogles2-m3-2-20261011-124206` (with the compiler: `m2_link` 29/29 including the debug log, everything else as before, warm cache without compiles) and `ogles2-m2-4-20261011-124258` (without: all pass).
- **M3 exit criteria all met:** output identical to `v3dglslc`; under 1 s per program after a first compile of 1.6 s; no pre-filled cache needed; real compile and link logs. **Left for later, by decision:** the compiler's size (19 MB, loaded in under a second; trimming Mesa's rule tables would have to be mirrored in the offline compiler to keep the output identical, so it waits until size matters) and the installer (`LIBS:v3dglslc` and an `OGLES2Cache:` assign, with the release, M7).

**Done (2026-10-11): M4, first part: no dropped frames, and a GPU that recovers.**
- **The binning list grows** (`ogles2/job.c`): its 4 MB block is followed, when full, by a new block (twice the last size) linked with a `BRANCH` packet written little-endian in 8 bytes kept free at every block's end, as Mesa's `v3d_cl_ensure_space` does; the list is submitted from the first block to the end of the last; finished blocks are flushed to the GPU when left and freed when the frame has rendered. v3dcore gained the hook for it (`v3d_static_buffer.grow`, called by `v3d_buffer_claim_memory` before its overflow fallback). The state buffer already grew; the render and tile lists do not depend on the number of draws.
- **More tile-list memory:** the binner's out-of-memory blocks (256 KB each) are allowed up to 96 per job instead of 8 (24 MB of tile lists), with a separate pool of 8 kept for reuse, and the retire list holds 128 blocks instead of 32.
- **Waits that watch progress:** the binning and render waits count polls only while the control list executor's current address (`CT0CA`/`CT1CA`) stands still, and the render bound is never below 300,000 polls (it was scaled down to 8,192 for a 256x128 target, a few milliseconds).
- **GPU reset** (`v3d_gpu_reset` in v3dcore, Linux's bcm2835-power sequence: AXI bridges stopped, `PM_GRAFX` reset line pulsed, `power_on_V3D`, performance counters set up again): after any binning or render job the GPU did not finish, and at device open when control list thread 0 is found running with its address standing still (left so by an earlier program).
- **Found the hard way:** the first stress run hit the old limit of 8 out-of-memory blocks; v3dcore then gave up on the job while the binner still waited for memory, and **thread 0 stayed stopped across three Amiga reboots: rebooting the Amiga under Emu68 does not reset the Raspberry Pi or its GPU.** Every later job failed until the library learned to reset it; the new library found the stopped thread at device open, reset it, and the GPU worked again without switching the Pi off (`ogles2-m4-1-20261011-124733` and the runs after it).
- **A1200 run `ogles2-m4-1-20261011-125816`**, remote: `m4_stress` 5/5 with one frame of 100,000, 250,000 and 400,000 draws (the last two continue the binning list in a second block; 85,000 to 90,000 draws a second issued), every column of the bitmap in its last draw's colour, the next frame clean, the memory returned; `m2_buffers` 27/27, `m2_elements` 13/13, `m2_link` 27/27, `m2_texture` and `m1_draw` 23/23 each; **no frame dropped, no timeout, no reset**, 0 bytes of GPU memory left. The corpus again 11/11 (`ogles2-m2-5-20261011-125849`).

**Done (2026-10-11): M4, second part: copies from the framebuffer, ETC1.**
- **`glCopyTexImage2D`, `glCopyTexSubImage2D`** (`texture.c`): the region is read as `glReadPixels` reads it (the open job rendered first, rows bottom-up, which is a texture's row order too), converted to the internal format by GLES 2.0 table 3.15 (LUMINANCE takes red, ALPHA alpha, RGB drops alpha), and given to `glTexImage2D`/`glTexSubImage2D`, so targets, cube faces, levels and errors are theirs. Each level now keeps its internal format: `glTexSubImage2D` refuses another format (`GL_INVALID_OPERATION`, GLES 2.0 3.7.2, not checked before), and a copy into a level converts to that level's format.
- **`OES_compressed_ETC1_RGB8_texture`:** `glCompressedTexImage2D` decodes ETC1 into the RGBA8 storage (`ogles2/etc1.c`); the level keeps the ETC1 format, so `glTexSubImage2D` and `glCompressedTexSubImage2D` refuse it as the extension says; `GL_COMPRESSED_TEXTURE_FORMATS` lists it. **The decoder matches Mesa's on 20,169 decodes** (random blocks of both modes and both flips, every image size from 1x1 to 13x13; `make etc1-test`, also in CI), after one fix: a differential base outside 0..31, which ETC1 leaves undefined, is now computed in a byte as Mesa does.
- **A1200 run `ogles2-m4-1-20261011-131117`:** `m2_texture` 29/29 (new: copies across a colour boundary, a LUMINANCE copy, a sub-copy, their errors; an ETC1 block decoded to the expected colours, the extension queries, ETC1's errors), the stress test and the other tests as before; 0 bytes of GPU memory left. **133 of 148 entries are implemented**; the 15 left are framebuffer objects (M5, 14) and `aglSetBitmap`.

**Done (2026-10-11): M4, third part: native texture formats, BGRA8888, texture renaming** (§7.6 "As built").
- v3dcore: `V3DTexture.cpp`, `v3d_texture_alloc_cpp` and `v3d_texture_store` (any box, Mesa's stride and padded-height convention); ogles2: the storage format per texture, packing at upload, the texture state's type and swizzle from it; each level also keeps the type it was given in.
- `EXT_texture_format_BGRA8888` (`GL_BGRA_EXT`, unsigned bytes).
- **Renaming** replaces the mid-frame render when a texture the open job used changes (`glTexImage2D`, `glTexSubImage2D`, `glGenerateMipmap`, the copies, deletion); the new copy is built at the next upload or, at the latest, at the next draw that samples it.
- **A1200 runs:** `ogles2-m4-1-20261011-131948`: `m2_texture` 33/33 (new: 100x60 RGBA8 and 50x30 RGB565 non-power-of-two textures sampled in all four quadrants; a texture changed mid-frame keeps the frame's depth, its sub-image update arriving too; BGRA8888; every earlier format check now through native storage), the stress test and the other tests as before; `ogles2-m2-5-20261011-132023`: the corpus 11/11; 0 bytes of GPU memory left.

**Done (2026-10-11): M4, fourth part: depth and stencil across a mid-frame flush** (§7.2).
- A job that ends before its frame (`glReadPixels`, the copies, `glFlush`, `glFinish`, the renaming fallback) stores depth and stencil to memory of the context's (D24S8 as one `ZSTENCIL` buffer, D16 and D32F as `Z`; UIF without XOR, padded as Mesa pads a UIF level), and the next job of the frame loads them instead of clearing; `aglSwapBuffers` keeps nothing (EGL: undefined after a swap). A depth-only or stencil-only clear after such a flush is drawn as a rectangle, so the other one stays.
- **Found on the A1200, both from Mesa's `v3dx_rcl.c`:** the early Z/S clear may only be used by a job that neither loads nor stores Z/S (with it, the stored depth was always the clear value; the stencil survived); without it, the clear packets (the dummy tile at the render list's start and the end-of-tile clear) must clear Z/S too, or the tile buffer keeps the previous tile's depth and stencil.
- `ogles2_job_flush(ctx, frame_end)`; a new test `m4_depth` (D16 and D24S8: depth across `glReadPixels`, stencil across `glFlush`, a depth-only clear after `glFinish` keeping the stencil, the next frame starting from its own clears).
- **A1200 runs:** `ogles2-m4-1-20261011-133100`: `m4_depth` 12/12, the stress test and every other test as before; `ogles2-m2-5-20261011-133123`: the corpus 11/11; 0 bytes of GPU memory left.

**Done (2026-10-11): M4, fifth part: partial buffer updates, `OES_mapbuffer`** (§7.5).
- `glBufferSubData` patches each GPU copy the open job does not read, converting only the touched elements (each copy now remembers its element size and draw.c's converter); `OES_mapbuffer`'s three entry points, the first extension commands in the dispatch table (`gen/api.txt` `command` lines, appended to `abi_order.txt` with `gen_ogles2.py --append`: 151 entries now).
- **A1200 run `ogles2-m4-1-20261011-133450`:** `m2_buffers` 32/32 (new: quad 2's colours changed between frames while the others keep theirs; map, write through the pointer, unmap, draw; the mapping queries; a second map refused), every other test as before; 0 bytes of GPU memory left. `make vbcc` builds the vbcc client with the new entries. **Implemented: 133 of the 148 core entries and the 3 `OES_mapbuffer` entries.**

**Done (2026-10-11): M4, sixth part: `aglSetBitmap`** (§6.4).
- `ogles2_display_set_bitmap` (display.c) checks the new bitmap as at creation; `ogles2_device_context_resize` wraps `v3d_context_resize` and goes back to the old size if the new one cannot be allocated; the depth/stencil save memory of the old size is freed. A new test `m4_setbitmap` (hidden bitmaps: 256x128 to 160x96 and back, depth across `glReadPixels` at the new size, the frame reaching the new bitmap's corners, the old bitmap untouched, a planar bitmap refused, a same-size switch, an unswapped frame going to the old bitmap).
- **A1200 run `ogles2-m4-1-20261011-134050`:** `m4_setbitmap` 14/14, every other test as before; 0 bytes of GPU memory left. **Every core entry point but the 14 framebuffer-object ones is implemented (134 of 148), plus `OES_mapbuffer`.**

**Done (2026-10-11): M4, seventh part: the speed comparison with MiniGL** (§9 M4 exit).
- `tests/perf`: Brian Paul's gears scene (glxgears' geometry as independent triangles, its flat faces with their face normals; one directional light, diffuse plus GL's global ambient 0.2) drawn by `gears_gles2` (ogles2: a buffer object per gear, or client arrays; one `glDrawArrays` per gear; lighting in the vertex shader, `tests/common/lit_shaders.h`) and by `gears_mgl` (PiStorm3D 29.1's minigl.library: immediate mode, vertex arrays, or display lists as glxgears uses them; fixed-function lighting on the CPU). Same 640x480 size, same target kind (a hidden BGRA32 bitmap, or a Workbench window: both libraries `ClipBlit` to it), no vsync, 5 seconds each after 5 untimed frames. `TEETH=n` multiplies the teeth: 800, 8,000 or 32,000 triangles. `make perf-mgl` builds the MiniGL side against the PiStorm3D 29.1 SDK (`MGL_SDK`); package `m4-2`.
- **The same picture:** after the timed frames each program draws one frame at angle 0 and counts the mostly red, green and blue pixels; ogles2 and MiniGL agree within 5 pixels of about 114,000 at every size, and the window run's screenshots show the same gears.
- **A1200 run `ogles2-m4-2-20261011-141440`** (frames per second, with the render-target cache maintenance below; screenshots during a run cost about 15% throughout):

| Triangles | ogles2 VBO | ogles2 client arrays | MiniGL immediate | MiniGL vertex arrays | MiniGL display lists | VBO / MiniGL's best |
|---|---|---|---|---|---|---|
| 800 | 833.8 | 638.2 | 553.2 | 484.4 | 668.8 | 1.25x |
| 8,000 | 649.0 | 184.4 | 119.2 | 89.4 | 151.4 | 4.3x |
| 32,000 | 410.8 | 57.4 | 33.8 | 25.0 | 47.2 | 8.7x |
| 800, window | 456.8 | | | | 401.6 | 1.14x |
| 8,000, window | 395.0 | | | | 133.0 | 3.0x |

  Before the cache maintenance (run `ogles2-m4-2-20261011-135013`) the VBO path made 984.6, 703.2 and 418.2 fps on the bitmap and 485.8 and 415.4 in the window: it costs about 0.2 ms a frame. MiniGL leaves its equivalent off by default (`MGLV3D_WINDOW_CACHE_PAIR`), so its numbers do not include it.

- **Reading it:** at 800 triangles both are bound by per-frame costs (one job, its wait, the copy to the window), so the gap is small; with more vertices MiniGL's CPU transform and lighting dominate while the VBO path barely slows (985 to 418 fps for 40 times the triangles). Client arrays pay for the conversion of every vertex every draw and land just above MiniGL. A real application with many small draws (es2gears: one `glDrawArrays` per strip) pays ogles2's per-draw cost more often; that is the next thing to measure if performance work continues (M6).

**Done (2026-10-11): M4, eighth part: `EXT_blend_minmax`, `OES_draw_elements_base_vertex`, the render target and the CPU cache** (Appendix B.3, §7.2).
- `GL_MIN_EXT`/`GL_MAX_EXT` map to the hardware's MIN and MAX blend modes, with the factors forced to ONE as Mesa's state tracker does (`st_atom_blend.c`: GL ignores them, the hardware would not). `glDrawElementsBaseVertexOES`: the copied vertex range moves by the base vertex and the rebased indices stay as they are, so no packet is needed (the 152nd dispatch entry, appended). `m2_elements` 22/22 (new: base vertex 4 and -4, from client and buffer indices; MIN and MAX over grey with factors ZERO).
- **Found on the A1200:** `m4_setbitmap` failed 3 runs in 5 with one pixel of a frame still showing the previous frame. The GPU writes the render bitmap by DMA, past the CPU's caches; a cache line the CPU had read before keeps the old bytes (PiStorm3D's `context.c` describes the same "stale 64-byte CPU cache lines of the previous frame" for its window copy). Each job now cleans the bitmap's lines (`CachePreDMA`) before the render and invalidates them (`CachePostDMA`) after, while locked, wherever the CPU reads the target: an application bitmap and the window mode's copy. A fullscreen screen is read by the display, not the CPU, so only `glReadPixels` invalidates the rows it reads (`ogles2_display_dma`, `ogles2_display_read`). After the fix: 5 runs in 5 clean.
- **A1200 runs `ogles2-m4-1-20261011-141142` to `-141231`:** every test passes in all five. Seen in some runs, before and after the fix: a test's "destroy returns the memory" check off by about 20 KB of CPU memory (20,304 bytes twice, once 132,592) with 0 bytes of GPU memory left, in different tests each time: something else on the machine allocating during the test (the test station's server, the system monitor), not ogles2; the tests' 16 KB tolerance is tight for a busy machine.

**Done (2026-10-11): M4 closed.** Its first exit criterion counted the 14 framebuffer-object entry points, M5's scope; M4 closed with them moved there (§9).

**Done (2026-10-11): M5, framebuffer objects. M5 COMPLETE** (§7.7 "As built (M5)").
- `fbo.c` (new): framebuffer and renderbuffer objects, their 14 entry points, the GLES 2.0 4.4.5 completeness rules and queries; colour images: texture levels (any level, `OES_fbo_render_mipmap`; cube faces) stored as RGBA8, RGB565, RGBA4 or RGB5_A1, and colour renderbuffers; depth and stencil: DEPTH_COMPONENT16, STENCIL_INDEX8, DEPTH24_STENCIL8 (`OES_packed_depth_stencil`), together or D16 and S8 apart. `glReadPixels` and `glCopyTex*` from an object; `EXT_discard_framebuffer`.
- `job.c`: a job belongs to one framebuffer; an object's render list after Mesa's `v3dx_rcl.c`; tile pools of its own when larger than the default framebuffer. `draw.c`: the Y flip only for the default framebuffer (viewport, clip window, scissor, front face, `gl_FragCoord` and `gl_PointCoord` transforms as Mesa's `prog_statevars.c`). `texture.c`: levels the GPU drew into are read back (`v3d_texture_load`, new in v3dcore) before their RAM copy is used; `GL_RGB` textures sample alpha 1.
- **153 dispatch entries, all implemented: the 148 core entry points (142 `gl*`, 6 `agl*`) and 5 extension ones.**
- **Tests:** `m5_fbo` 44/44 (objects and queries; render to texture read back and sampled, with its orientation; an object's pass in the middle of a screen frame, the screen's depth kept; D16 depth kept between jobs; stencil in D24S8 and in D16 + S8; the four colour renderbuffer formats; level 1, a cube face, `glTexSubImage2D` and `glGenerateMipmap` after rendering, `glCopyTexImage2D` from an object; a 512x512 object with its own pools; incompleteness and errors; 40 objects made, drawn into, sampled and deleted; discard). The corpus has three programs of our own for M5 in `tests/corpus/programs` (neither of its sources uses framebuffer objects): `RTT_Cube` (render to texture, depth renderbuffer), `PostFX` (scene, horizontal blur, vertical blur and composite: three framebuffers a frame) and `Orientation` (gradient from `gl_FragCoord`, culling with both windings, point sprites from `gl_PointCoord`, a corner marker: drawn into an object and shown, and drawn onto the screen, side by side). On the PC, binding framebuffer 0 binds the capture object (`host_hooks.h`).
- **A1200 runs:** `ogles2-m5-1-20261011-144423`: `m5_fbo` 44/44, every earlier test as before, 0 bytes of GPU memory left; `ogles2-m2-5-20261011-144235`: the corpus 14/14, the three new programs with 0.00% of their pixels different from llvmpipe's.

**Done (2026-10-11): M6, first part: where a frame's time goes, and CPU/GPU overlap** (§7.1, §7.2 "As built (M6)").
- **Timing in the library:** each context sums, per job, the CPU's part before the flush, building the render list, the cache work before and after the GPU, and the waits for binning and render; per frame, the time between swaps and the presentation; `aglDestroyContext` logs the averages. The clock is the Pi's 1 MHz system timer (peripheral offset 0x3004, one read of about 50 ns); `timer.device`'s E-clock reads the Amiga's CIAs across the PiStorm bus, about 25 microseconds a call, which made the measurement cost 15% of the frame rate.
- **Found:** every exec cache call (`CachePreDMA`, `CachePostDMA`, `CacheClearE`) costs about 41 microseconds whatever its range, 64 bytes or 1 MB, clean or dirty (`tests/perf/cachetime.c`): on Emu68 they clean the whole data cache. A flush made five before the GPU (four control-list buffers and the target). Now one `CacheClearE(0, ~0, CACRF_ClearD)` (the whole data cache, data only: an instruction-cache clear would make Emu68 drop its translated code) before the GPU, and the invalidate after the render only where the CPU reads the target; `glBufferSubData`'s patches and new buffer copies no longer call the cache at all (the next flush cleans).
- **Overlap:** while it is the only ogles2 context, a flush returns once the render is submitted when nothing needs the finished frame at once (fullscreen swaps, framebuffer-object jobs, mid-frame flushes). The job keeps the GPU and bitmap locks until `ogles2_job_finish`: at the next flush, or before the CPU reads what the render writes (`glReadPixels`, `glFinish`, read-backs) or changes what it reads (a texture or buffer copy it uses is renamed, as for the open job; a texture it renders into, tile pools, `aglSetBitmap`, destroy wait). `fbo.c`, `texture.c` and `buffer.c` ask `ogles2_job_uses` for both jobs.
- **The gears comparison gained a fullscreen step** (`FULLSCREEN`, each library's own 640x480 screen; ogles2's mode searched for BGRA32/RGBA32, since BestCModeID offers ARGB32, which the tile store cannot write).
- **A1200 run `ogles2-m4-2-20261011-151109`** (frames per second; per-frame breakdown in its `ogles2.log`):

| Gears, ogles2 vertex buffers | before (M4) | now | MiniGL's best |
|---|---|---|---|
| bitmap, 800 triangles | 833.8 | 964.4 | 674.6 |
| bitmap, 32,000 triangles | 410.8 | 445.2 | 47.4 |
| window, 800 triangles | 456.8 | 497.8 | 401.0 |
| screen, 800 triangles | (no mode) | **1734.0** | 791.0 |
| screen, 8,000 triangles | (no mode) | **1171.2** | 161.4 |

  On the screen the render wait fell from 472 to 2 microseconds a frame: the GPU renders while the CPU builds the next frame. A bitmap frame (800 triangles, 1036 us) is now 259 us of the program's own work, 63 building the render list, 57 cleaning the cache, 25 binning, 472 rendering, 59 invalidating; a window frame adds about 1 ms of `ClipBlit`.
- **Regression runs:** `ogles2-m5-1-20261011-151104` every test passes; `ogles2-m2-5-20261011-151344` the corpus 14/14.

**Done (2026-10-11): M6, second part: gl4es** (`ports/gl4es`, its README).
- gl4es (desktop OpenGL 2.1 on GLES2, MIT) at the referenced commit, built unchanged with its AmigaOS 4 code paths by GCC 6.5 and clib2 (`make gl4es`: `libgl4es.a`, 1.5 MB). The Amiga's a.out objects have no `alias` attribute, which gl4es exports every GL name with: a force-included header turns those into assembler equates. Its AmigaOS 4 GLES2 glue (`IOGLES2`) is replaced by one generated from ogles2's dispatch table (`gen_glue.py`, 147 functions, extensions included), and its `agl.c` by `agl_os3.c` (gl4es started at the first context, no constructor). libogles2.a's `gl*` are inline wrappers, so gl4es's exported `gl*` names meet nothing at link time.
- `gears_gl4es`: the gears comparison's MiniGL program built on gl4es (`GEARS_GL4ES`): glxgears' scene through `glBegin`, vertex arrays or display lists with fixed-function lighting. Package `m6-1` brings the on-target compiler (gl4es writes its shaders at run time) and a shader cache.
- **A1200 run `ogles2-m6-1-20261011-153100`:** every step draws the gears correctly (the pixel counts of ogles2's and MiniGL's renders exactly; screenshots of the window); frames per second at 640x480, 800 triangles: immediate mode 157, vertex arrays 548, display lists 522 on a bitmap (MiniGL 563, 490, 675), 341 in a window (MiniGL 401), 714 full screen (MiniGL 791); 8,000 triangles: vertex arrays 148 (MiniGL's best 151). gl4es's start-up compiles three probe shaders for optional GLSL features that fail by design, and finds that the compiler takes GLSL ES 3.00. Its immediate mode costs about 2 microseconds of 68k time a vertex.

**Done (2026-10-11): M6, third part: Quake III Arena on gl4es** (`ports/ioq3`, package `m6-2`). M6's gl4es criterion is met: gl4es's gears and Quake run.
- ioquake3 for AmigaOS 3 as the MiniGL build of it (the user's `Quake3/MiniGL_Library_Quake3/ioq3`, unchanged: the port copies it and overlays its own `amiga_glimp.c` and a `<mgl/gl.h>` that brings gl4es's GL; since 2026-10-12 the copy also gets `ports/ioq3/patches`). The game data comes from the user's installation; nothing is written there (`fs_homepath` is the package's).
- **Exit hang (three reboots):** a fullscreen job left running across the next frame (overlap, §7.2) also held the screen's bitmap lock; P96 draws the mouse pointer into that bitmap, so input.device blocked and Quake's input-handler removal never returned. A screen of ogles2's own is now unlocked as soon as its render is submitted (it is on display and does not move); only a framebuffer object's job or one on ogles2's own screen is left running.
- **Where a frame goes** (probe P, gl4es's glue with `GL4ES_GLUE_PROFILE`, the port's draw timing; demo `four`, 640x480): 2,100 draws a frame, nearly all `glDrawElements` from client arrays; 86 of the frame's 106 ms in those draws, 47 in ogles2 (22 us a draw) and 39 in gl4es (19 us a draw: its fixed-function emulation, `realize_glenv`, per draw); Quake's own work about 20 ms; the GPU idle almost all the time. A draw copies little (about 400 bytes of vertices, a few indices); its cost was fixed bookkeeping. `tests/perf/emuspeed` timed the pieces on the A1200: Emu68 runs this code at 3 to 5 ns a 68k instruction (a call 48 ns, a byte-swapped word 11 ns, a 16-byte packet cleared a byte at a time 160 ns, a timer read 56 ns).
- **Made faster (ogles2's draw, 29.5 to 12.3 us):** a texture's state records are reused by a job's later draws (checked before the completeness test); a program keeps its GL shader state record as a template, its variants and code addresses per primitive mode, its sampler list and its slot-to-attribute map; the coordinate and vertex shaders' uniform streams are reused while the vertex constant buffer (a version per stage) and the viewport, depth range, line width and flip are the same (the fragment shader's samples textures and is always written), and the constant buffers are kept little-endian beside the native copy so a stream is a copy through an index table made at load; `emit_state` is skipped while a state version (bumped by every setter that changes something, glEnable/glDisable only on a change) and the framebuffer are the same; the five packets that end every draw and the attribute records are written as bytes, checked once against v3dcore's writers (on a mismatch the library logs it and keeps v3dcore's); vertex conversion reads words directly with unrolled loops; the library's memcpy/memset/memcmp move long words. A first attempt at reusing uniform streams compared 33-word tables with a byte-at-a-time memcmp per draw and cost more than it saved.
- **Against MiniGL** (the user's `quake3`, minigl.library 29.1, the same command line; `fs_homepath` in RAM:): the port's renderer defaults to `r_primitives 1` (Cowcat's), which splits every batch into triangle strips, one `glDrawElements` each: 2,100 draws a frame of about 4 indices. That is MiniGL's cheap path and gl4es's and ogles2's dear one. Timedemo `four`, 640x480: MiniGL 34.4 fps with `r_primitives 1`, 60.8 with 0; ogles2 through gl4es 12.1 with 1, **38.7 with 0** (`ogles2-m6-2-20261011-181127`: 130 draws a frame, 25.8 ms a frame: draws 9.3 ms, of them ogles2 4.3 and gl4es about 5; Quake about 16). Package `m6-2` now sets `r_primitives 0`. The MiniGL build is faster with it too.
- **Memory accesses are what Emu68 charges for** (`tests/perf/emuspeed`): 100 longs copied or byte-swapped a long at a time 1.1 us, with `movem` (four longs an instruction) 0.23 us; clib2's `memcpy` (movem) 0.1 us for 256 bytes, its `memcmp` (a byte at a time) 2.4 us; a loop pass with two branches 17 ns. So ogles2's vertex conversion, 32-bit index copy and `memcpy` use `movem`; the index range scan is branch-free (min and max through the sign bit); `ports/gl4es/os3_string.c` gives the program and gl4es a `memcmp` comparing long words (at even addresses: the m68k ABI aligns structs to 2). With `r_primitives 0`: **39.6 fps** (`ogles2-m6-2-20261011-183734`), ogles2's draws 3.6 ms a frame (28 us a draw of 144 indices and 1.6 KB of vertices). The gears through gl4es (`ogles2-m6-1-20261011-183851`, the same pixel counts): display lists 8,000 triangles 298 fps, full screen 972.
- **gl4es's draw, timed inside** (temporary timers in its source, removed): per frame `realize_textures` 1.55 ms, the fixed-function program's selection 1.25 ms (`fpe_program`: a ~300-byte state copied, compared and, when it differs, hashed a byte at a time), the built-in uniforms 1.34 ms, vertex attributes 0.3 ms; and Quake's 2,600 state calls a frame (enable, pointers, binds, matrices) 2.1 ms in gl4es. About 6.5 ms a frame against MiniGL's whole GL cost of about 2 ms.
- **gl4es patched** (`ports/gl4es/patches`, applied by `build.sh` to a clean checkout on every build): a mip chain the program uploads down to 1x1 counts as done (Quake III set its filter after uploading, and gl4es generated every chain again at the first draw: `realize_textures` 1.54 to 0.60 ms a frame, the generation's 1.2 s over the run gone; the textures' one GPU upload at first use stays, about 1 s); the fixed-function state hashed a long word at a time. Tried and dropped: sending the built-in matrices only after a matrix change, with `glLoadIdentity` of an identity a no-op (0.1 ms: Quake's renderer really changes them around most draws). Timedemo **40.2-40.5 fps**; gears unchanged in pixels. Per function, Quake's state calls a frame: `glDisable` 336 (0.45 ms), `glLoadIdentity` 488, `glMatrixMode` 497, `glEnableClientState` 284, `glBindTexture` 128 (0.2 ms each). What is left in the timedemo's average is partly one-time: the textures' first uploads (1 s) and, at start-up, gl4es's three probe shaders that fail by design (1.3 s: a refused shader left no record, so the compiler was started at every run).
- **One-time costs:** a shader the compiler refuses now gets a record too (`v3ds 0 <compiler id>` and its errors; the id is the compiler's size, found once while the library is open: a test package copies the compiler again every run, so not its date); gl4es's probes no longer start the compiler, 1.3 s off every start but the first. Texture upload: 32-bit texels into UIF are stored a whole UIF block (8x8) at a time with `movem` (v3dcore's `v3d_store_uif32_blocks`, checked once against the general path on a 64x256 test, which it replaces only while they agree); RGBA8 goes from the RAM copy without repacking; `v3d_texture_store` no longer calls CachePreDMA for every level (every job cleans the whole data cache before the GPU starts). The timedemo's worst frame 700 to 175-195 ms; **40.7-41.2 fps** (`ogles2-m6-2-20261011-210219`); m5-1 passes.
- **Loading:** `glTexImage2D` rebuilt the GPU texture at every new mip level, so level 0 was tiled and stored once for each level after it; the GPU copy is now built at the texture's first use. Quake's `CL_InitCGame` 21.5 to 10.2 s; the run's texture uploads 11 s to 0.4 s.
- **A1200 runs (`ogles2-m6-2-*`):** the timedemo 8.2 to 9.4 to 12.1 fps (1260 frames, 82.8 ms a frame; `ogles2-m6-2-20261011-180137`, probes on): per frame ogles2's draws 26 ms, gl4es about 38 ms (18 us a draw), Quake about 19 ms. The gears through gl4es (`ogles2-m6-1-20261011-175936`, the same pixel counts as before): immediate mode 157 to 351 fps, vertex arrays 548 to 707, 8,000 triangles 148 to 327, full screen 714 to 893; every run exits cleanly; GPU memory constant (59 MB), no textures renamed. The package runs about 230 frames by default (`FRAMES=400`, a 42 s run; `FRAMES=0` the whole demo). The user saw a few floor and wall polygons flicker to black in some places; no texture is sampled while incomplete (logged once a texture if one is), so the cause is still open (found on 2026-10-12: the game's colour array, below).

**Done (2026-10-11): M6, fourth part: SDL2** (`ports/sdl2`, its README; package `m6-3`).
- The base is Steffen Haeuser's SDL2_68k (Aminet `dev/lib/SDL_68k`, SDL 2.33.0, MiniGL backend) at a pinned commit; `ports/sdl2/build.sh` clones it, applies `patches/`, adds `SDL_os3opengles2.c` (from AmigaOS 4's `SDL_os4opengles2.c`) and builds `libSDL2.a` with clib2. The GL backend is chosen when SDL loads the GL library, by the profile asked for: ES 2.x gets ogles2.library, anything else minigl.library. SDL's GLES2 renderer is built in.
- **ogles2:** `aglSetParams2(OGLES2_CCT_WINDOW)` moves a context to another window or a resized one (a new offscreen bitmap and, when the size changes, new pools; the old ones kept until the new ones exist). A GimmeZeroZero window (SDL's) is blitted at its RastPort's origin; before, the picture came out a border lower.
- **Three crashes found on the way:** the test wrapper's first `SDL_AddTimer` thread outlived a program that left without `SDL_Quit` (now a thread-free `--wrap` of the present calls, `tests/run_for.c`); SDL emulated the window surface with a GLES2 texture inside the software renderer's creation (off on AmigaOS 3); and **bebbo GCC 6.5's inline headers load A6 before evaluating the arguments**, so a library call nested in another's arguments calls the outer one through the inner library's base: SDL2_68k's `AllocBitMap(..., GetCyberMapAttr(...), ...)` ran through CyberGfxBase (unpatched SDL2_68k built with this toolchain crashes the same way). Inner calls must be hoisted into locals; no other case in SDL2_68k's sources or ours.
- **A1200 run `ogles2-m6-3-20261011-222733`** (6 s a step, every step exits cleanly, 0 bytes of GPU memory left): `testgles2` in a window 450.6 fps, full screen 435.5; `testsprite2` GLES2 renderer 157.3 (software renderer 23.5); `testdraw2` 60.9 (478 draws a frame); `testrendercopyex` 19.6.
- **Render targets made and deleted every frame** (`testrendercopyex`: a new 640x480 target texture a frame; `tests/perf/rtt_churn`, step 7 of `m6-3`, makes the same GL calls and times each): `glDeleteTextures` took 38 ms. It cleared the texture's "rendered" marks and then waited for the job drawing into it, whose completion set them again, so the whole image was read back from the GPU just before it was freed; the wait now comes first. And a level made by `glTexImage2D` without pixels is "blank": the GPU copy, which starts cleared, is not stored from the zeroed RAM copy (1.2 MB). `rtt_churn` 20.4 to 144 fps (one target kept: 316); what is left is allocating and clearing the two copies (about 1.8 ms each).
- **A1200 runs:** `ogles2-m6-3-20261011-224131`: `testrendercopyex` **108.0 fps** (was 19.6), the rest as before (`testgles2` 472.2, sprites 167.0, software 25.1, `testdraw2` 59.9, full screen 464.0); `ogles2-m5-1-20261011-224244`: every test passes (`m5_fbo` 44/44).

**Done (2026-10-11): M6, fifth part: SDL3** (`ports/sdl3`, its README; package `m6-4`). M6's SDL criterion is met for both SDL versions.
- Steffen Haeuser's SDL3_68k (SDL 3.4.14, from capehill's AmigaOS 4 port, MiniGL backend) at a pinned commit, patched as SDL2_68k: the same backend with SDL3's API (`SDL_os3opengles2.c`), the backend chosen by profile at `GL_LoadLibrary`, SDL's GLES2 renderer built in. SDL3_68k's driver already keeps the window surface its own (no texture framebuffer).
- This toolchain's clib2 has a 2-byte `wchar_t` and GCC a 4-byte one, which SDL3's `SDL_string.c` rejects: clib2 objects are built with `-fshort-wchar`. On AmigaOS 3 `SDL_main.h` makes a `main()` only for programs with SDL3's main callbacks, so `run_for.c` starts those through a wrapped `SDL_RunApp` and the others as `main()`; SDL3's `testgles2` needs `HAVE_OPENGLES2`, as SDL2's.
- **A1200 run `ogles2-m6-4-20261011-225640`** (every step exits cleanly, 0 bytes of GPU memory left): `testgles2` in a window 453.7 fps, full screen 432.0 (SDL3_68k's fullscreen screen keeps its title bar: a 640x466 window); `testsprite` GLES2 renderer 250.7 (software 199.7); `testrendercopyex` 88.8; `testdraw` 109.8.

**Done (2026-10-11): M6, per-draw CPU time, continued.**
- gl4es patch `0003-fpe-program-unchanged`: `fpe_program` ran `fpe_ReleventState` (bitfield clears for all 16 texture units) every draw; it now remembers the raw states of its last four programs (16 measured slower: the comparisons cost). Timedemo `four` **41.9 fps** (`ogles2-m6-2-20261011-231541`); gl4es and ogles2's draws 7.8 to 7.3 ms a frame. The gears through gl4es (`ogles2-m6-1-20261011-231700`): the same pixel counts.
- **1600x1200, highest settings** (package m6-2's `Bench-Max`, `max.cfg` as each run's autoexec.cfg: 32-bit textures at full size, trilinear, `r_lodbias -2`, `r_subdivisions 1`, dynamic lights, flares, detail textures, shadows; `r_primitives 0`; results `ogles2-m6-2-max-20261011-233400`): timedemo `four` **gl4es on ogles2 33.7 fps, MiniGL 33.0 fps** (frame min/avg/max 11.0/29.7/188 ms and 14.0/30.3/60 ms: ogles2's worst frames are likely the textures' first GPU upload). Both fall to about the same rate: 6.25 times the pixels of 640x480, and more passes (detail textures, dynamic lights); how much of the frame is the GPU's was not measured in this run (no log).
- Where a Quake frame's GL time goes now (gl4es timers, removed again; per frame, 130 draws): ogles2's `glDrawElements` 3.7 ms (29 us a draw: the index range scan and checks 4.5 us, texture records 4.1, uniform streams 3.8, vertex data 6.3, binning list 4.3); in gl4es the matrices 0.7 ms, the program choice 0.6, texture realisation 0.6, other uniforms 0.45, vertex attributes 0.3; Quake's 2,600 state calls 2.1 ms. Quake's own work is about 14 ms of the 23.8 ms frame; MiniGL's whole frame is 16.4 ms.

**Done (2026-10-12): per-draw time, and sharing** (written while the A1200 was offline, then run).
- **A1200 runs:** `ogles2-m5-1-20261012-101446`: every test passes, `m6_share` 25/25 (the 20 rounds of A rendering and B sampling with the overlap on: 0 wrong), `m2_elements` 29/29 with the range checks; `ogles2-m2-5-20261012-101525`: the corpus 14/14 against llvmpipe; `ogles2-m6-1-20261012-101611`: the gears through gl4es draw the same pixels (display lists 698 fps, 8,000 triangles 324, window 414); `ogles2-m6-2-20261012-101753`: timedemo `four` **42.9 fps** (was 41.9), gl4es and ogles2's draws 7.3 to 6.6 ms a frame (ogles2's checks 4.5 to 2.1 us a draw, its texture records 4.1 to 2.9); `ogles2-m6-3-20261012-101859` (SDL2): `testgles2` 542, sprites 193, software 27, `testrendercopyex` 127, `testdraw2` 72 fps (were 472, 167, 25, 108, 60); `ogles2-m6-4-20261012-102007` (SDL3): 511, 276, 235, 101, 126 fps (were 454, 251, 200, 89, 110). Every context freed its GPU memory.
- **Index range given, not scanned:** ogles2 1.1 appends GLES 3.0's `glDrawRangeElements` to the dispatch table (offered through `aglGetProcAddress`); `draw_elements` takes the range instead of scanning the indices. gl4es patch `0004-locked-range-draw` sends a draw of the program's own locked arrays there with the locked range (Quake III locks every batch: about 3.5 us a draw). `m2_elements` checks it (8-, 16- and 32-bit indices, a range wider than the indices, an element buffer, end before start).
- **Texture records kept per texture:** a texture's texture and sampler state records live in a 64-byte block of its own and hold across jobs while its parameters and GPU copy are the same (before: rebuilt in every job's state buffer, 2.5 us each; most of a Quake frame's textures are drawn once a frame). A record a job not yet rendered may read is not rewritten: the new one goes into that job's state buffer as before. The block goes with the GPU copy (deferred when renamed); an image change makes the records again (a new level can change the level count).
- **Sharing (`OGLES2_CCT_SHARE_WITH`, `share.c`):** every context is in a share group (alone without the tag) holding the name tables of textures, buffers, shaders and programs (and the program cache's images) and renderbuffers; framebuffers, the default textures and every binding stay per context. Job serials are unique across contexts (library-wide), so "a job not rendered yet uses it" looks at every member's open and running job, and a block such a job reads is freed after that member's render. A deleted object is detached in every member (GL keeps it while bound elsewhere: a deliberate difference); a deleted program lives while any member has it current; the shared objects go with the group's last member. The members are used from one task (logged once otherwise). Records kept on shared objects for one job (a texture's records in a job's state buffer, a program's reused uniform streams) note the context.
- **Several contexts with overlap:** a render may be left running (an object's job, the own screen) while every context with a GPU belongs to one task, not only while one context exists; before a context submits, a render another context of the task left running is waited for first (exec semaphores nest per task: two jobs would be on the GPU). Across programs phase 1 stays: the lock is held until the render is waited for, at the task's next flush, and a fair turn across tasks would block a program whose tasks wait for each other; that needs the shared submission component (D3).
- **Tests:** `m6_share` (in package m5-1, step 7): names shared with B and not with C; B draws with A's program, buffer and texture; B changes a texture A's open job reads (A shows the old image); A renders into a texture and leaves it running while B samples it, 20 times; deletes across the group; A destroyed first. Also to run: m5-1 and m2-5 (regressions), m6-1 (gears through gl4es), m6-2 with `FRAMES=0` (the timedemo: the range and the records), m6-3 and m6-4 (SDL).

**Done (2026-10-12): Quake's flicker, sharing across tasks, per-draw and per-job time.**
- **Quake's flicker found: the game's colour array, not ogles2 or gl4es.** In matched timedemo frames (Quake's `screenshot` at fixed frames of demo `four`, gl4es against the installed MiniGL build), 2 or 3 of 4 frames differed in 10-50% of the pixels: world surfaces dark, gold or black. Only with `r_ext_multitexture 1`; the same with the previous day's library and without gl4es patches 0003-0005. A temporary per-draw log (each draw's textures per unit, state records, uniform stream and arrays, written out at the frame Quake's `glReadPixels` reads) showed every binding and record right, but the multitexture draws' colour array still pointing at the previous surface's dynamic-light pass (`ProjectDlightTexture`'s `colorArray`, the rocket's orange `FF BF 00`): blue went to zero, red and green dropped. The port tree's AMIGAOS branch of `RB_IterateStagesGeneric` calls `DrawMultitextured` without setting the stage's colours (the other branch sets them first). `ports/ioq3/patches/0001-multitexture-stage-colours.patch` sets them; `ports/ioq3/build.sh` applies `patches/` to its copy (the user's tree is unchanged, and has the same code). The four frames now match MiniGL's within 0.7-1.5% (the console's message timing). The installed MiniGL build does not show it.
- **Per-draw CPU time:** gl4es patch `0005-builtin-uniforms-unchanged` (the built-in matrices sent only after a matrix value changed, by a generation `matrix.c` bumps; no uniform calls for units the program lacks): 42.9 to 43.2 fps. ogles2's state calls are a few hundred nanoseconds each; vertex conversion was already `movem`; gl4es's immediate mode already merges consecutive `glBegin`/`glEnd` blocks: left as they are.
- **CPU time per job:** `v3d_texture_store` already made no cache call (the item was out of date); a framebuffer object's job no longer runs `CachePostDMA` over its target after the render (41 us an exec cache call: the CPU reads the image only through v3dcore's `v3d_texture_load`, for a texture's sync, `glReadPixels` and `glCopyTex*`, which invalidates the level first; it was cleaned before the render with everything else); the supertile coordinate packets of the render list are written in one claim.
- **Sharing across tasks:** while a share group has two members or more, every dispatch-table entry that takes a context runs under the group's semaphore (`ctx->share_lock`, offset 4): `gen/gen_ogles2.py` writes for each entry a top-level assembler wrapper `ogles2_lk_*` that tests the context's magic and `share_lock` and jumps either to the entry or to an out-of-line `ogles2_sl_*` that takes the semaphore around it; a context alone pays the test (the wrappers cost about 0.3 fps in the timedemo, A/B). Pure state setters (`glEnable`, the blend, depth, stencil and viewport calls, `glActiveTexture`, `glGetError`; `NO_GROUP_LOCK`) touch only their context and go straight to it. `aglCreateContext2` joins and `aglDestroyContext` leaves under it. `OGLES2_CCT_SHARE_WITH` with a context that is not valid fails with `OGLES2_CCE_BAD_TAGS`. The current context of `<proto/ogles2.h>`'s calls is per program, so a second task calls through `OGLES2Dispatch` with its context.
- **A fair GPU turn across programs:** a render left running (an object's job, the own screen) now gives up the GPU submission lock right after it is submitted, with the library noting the context, task and render frame number; the next submitter of any program takes the lock and, if a render is still noted, waits for it: its own task finishes the job as before; another task waits for the frame number on the GPU (`v3d_wait_render_timeout`) and marks the job done for its owner (`pend_done`), which then only retires it. A program that holds the lock no longer blocks another until its next flush.
- **Tests:** `m6_tasks` (package m5-1, step 7; a child process from `CreateNewProcTags` with a context in the main task's group, calling through `OGLES2Dispatch`), 9 checks: the child changes a texture shared with the main task's context, and makes and deletes textures (the name table grows), while the main task draws with it: every frame shows one of the colours written, none dropped; both tasks render into framebuffer objects of their own contexts (renders left running) and read each back: every one right, nobody waits out the timeout.
- **A1200 runs (the final library, diagnostics removed):** `ogles2-m5-1-20261012-143525`: every test passes, `m6_share` 25/25, `m6_tasks` 9/9, each test's last context frees all GPU memory; `ogles2-m2-5-20261012-143551`: the corpus 14/14 against llvmpipe; `ogles2-m6-1-20261012-143638`: the gears through gl4es draw the same pixels (display lists 706 fps, vertex arrays 755, 8,000 triangles 379, window 412, full screen 1044); `ogles2-m6-2-20261012-143818`: timedemo `four` **42.8 fps** (frame min/avg/max 10.0/23.3/186 ms; within the session's drift of 42.1-43.2), 131 draws a frame, 914 calls into ogles2 taking 4.7 ms; `ogles2-m6-3-20261012-143923` (SDL2): `testgles2` 549, sprites 198, software 30, `testrendercopyex` 131, `testdraw2` 72, full screen 489 fps, `rtt_churn` 150 (one target 332); `ogles2-m6-4-20261012-144032` (SDL3): 510, 283, 235, 102, 130, full screen 452. Every program exits cleanly.

**Done (2026-10-12): Quake III without gl4es** (`ports/ioq3-gl2`, its README; package `m6-5`; plan `docs/IOQ3_GLES2_PORT_PLAN.md`).
- **Survey:** upstream ioquake3's `renderergl2` and the Amiga engine share `REF_API_VERSION 8`, `refexport_t` and the shared types (layout-identical); upstream's `refimport_t` has ten more members, filled by an adapter (`ioq3_gl2_ref.c`), so the engine is unchanged. The ES 2.0 path loads 73 GL functions, all in ogles2.library; it uses no framebuffer objects, no VAOs, and probes five optional extensions (only `GL_OES_element_index_uint` present). Its 109 GLSL programs (48 generic, 42 lightall, 19 others with the default cvars) all compile with v3dglslc on the PC.
- **Build:** the engine's objects and `.dll`s from `ports/ioq3`; the renderer copied unchanged, compiled against the engine's headers with a small compatibility header, GL headers from gl4es's include directory (headers only) and an `SDL.h` shim; a new glimp (`amiga_glimp_gl2.c`: the context as `ports/ioq3`'s, the GL functions by name through libogles2.a's `aglGetProcAddress`). `quake3-gl2` 1.7 MB (the gl4es build 2.7 MB).
- **First start:** the 109 programs compile on the A1200 in 107 s, then load from the cache in 1.4 s.
- **Buffers streamed per draw (ogles2):** renderergl2 orphans its 64 KB dynamic vertex buffer with `glBufferData(NULL)` and refills each attribute with `glBufferSubData` at every draw. ogles2 allocated and cleared a new shadow, dropped the GPU copies, and at the draw converted the whole buffer from each attribute's offset into a new GPU block: 60 ms a draw, one frame in 10 s (the network task starved). Now a buffer given as `GL_DYNAMIC_DRAW` or `GL_STREAM_DRAW` has no GPU copies: a draw converts the vertices it reads from the shadow into the job's state buffer, as from a client array; `glBufferData` of the same size keeps the shadow.
- **Animated models (ogles2):** renderergl2 keeps all of a model's frames in one static buffer and moves the attribute offset a frame at a time; ogles2 keyed its kept copies by the offset, so every frame of animation evicted a copy and converted the whole model again (frames of up to 4 s, GPU memory to 260 MB). A copy now holds the elements at one place within a vertex (offset modulo stride) and an array starts `offset / stride` elements into it: one copy per attribute for all frames.
- **Mouse pointer:** both Quake glimps had MiniGL's `mglSetPointer` and `mglClearPointer` the wrong way round (the engine calls the first when it takes the mouse): the pointer stayed on the screen.
- **Tests:** `m2_buffers` 34 checks: the partial update for a static and a dynamic buffer, and a static buffer's two "frames" by offset, alternating.
- **A1200 runs:** `ogles2-m6-5-20261012-153429`: timedemo `four` **76.1 fps** (1260 frames in 16.6 s; frame min/avg/max 5.0/13.1/93 ms), 117 draws a frame taking 3.7 ms in ogles2 (vertex data 1.2 ms), GPU memory 130 MB; MiniGL's build 60.8, the gl4es build 42.8. `ogles2-m5-1-20261012-153318`: every test passes. Four matched frames against MiniGL's: the same scenes, geometry, models, HUD; colours and lighting differ as renderergl2 lights per pixel; the blood decals differ (random); **open:** a score plum (the "1" sprite a frag raises, drawn with a 2D number shader) is missing in one frame, the depth pre-pass ruled out (frames byte-identical without it).

**Done (2026-10-12): Quake III on renderergl2, the remaining issues; sound.**
- **The missing score plum: a compiler bug.** Temporary logs (the renderer's sprite path, then ogles2's draw: state, the matrix, the vertices through it) showed the plum drawn with the scene's matrix at the right place, but its four vertex colours 0 (alpha 0). renderergl2 packs a colour as `c * 65535.0f + 0.5f` into a `uint16_t`; bebbo's GCC 6.5 (`-m68881`) converts a float to `short`, `unsigned short` or `unsigned int` with a bare `fmove`, without the `fintrz` C's truncation needs, so the FPU's rounding mode decides: round-to-nearest made 65535.5 into 65536, 0 in 16 bits (and normals of 1.0 into 32768, an overflow). About 130 such conversions in renderergl2 alone; none in ogles2.library (its assembly scanned). Emu68 honours FPCR (A1200 test): both Quake glimps first set round-to-zero for the game's task, which made those moves truncate. The cause is bebbo's optimiser pass `e` (§2.3), which the library already leaves out: the Quake ports now build with the library's `-fbbb=abcfhilmnprsz0` and the FPU mode is the default again (`ogles2-m6-5-20261012-224730`: 78.5 fps at 640x480, 44.7 at 1920x1080; the plum in the matched frames; m6-2 42.7 fps). The shipped caches were made again (the renderer prints `r_FBufScale` into the shaders, and the 640x480 digits differ by FPU mode).
- **Sound:** the engine's own drivers with the 2026-10-08 fixes (Paula's split stereo halves and signed silence; the AHI backend with Paula as fallback; build 296 of the port's tree, checked against the bug report) are in both builds. Playing the game, AHI's 16-bit sound works on the test machine; a timedemo's sound is garbled whatever the driver (the game runs faster than real time), which first looked like a silent or broken AHI. Both packages' `Play-Quake` now start with sound (the engine's default, AHI); m6-5's `Run-M1 SOUND=1` times the demo with sound: 55.8 fps with Paula (silent 75.8).
- **Window and `vid_restart`:** started in a window (`r_fullscreen 0`), switched to full screen by `toggle r_fullscreen` and `vid_restart` mid-demo, quit: every step clean (the context destroyed and made again).
- **A1200 runs:** `ogles2-m5-1-20261012-163506`: every test passes; `ogles2-m6-2-20261012-163605` (gl4es, with the FPU mode and pointer fixes): 43.2 fps; `ogles2-m6-5-20261012-163803`: 75.8 fps, `-163902` with sound 55.8.

**Done (2026-10-12): Quake III on renderergl2, finished** (phase 4 and 5 of `docs/IOQ3_GLES2_PORT_PLAN.md`).
- **Where a frame goes** (640x480, timedemo `four`; `Run-M1 SPEEDS=1`, Quake's `com_speeds`, and `PROFILE=1`, the GL glue timing every call): 13.9 ms a frame: the renderer's back end 9.7 ms (in it `glDrawElements` 3.8 ms for 118 draws, 32 us a draw; buffer updates 0.7 ms; uniforms about 0.4 ms), the client and cgame 2.5 ms, the renderer's front end 1.7 ms, events 0.3 ms. The glue is gl4es's (`ports/gl4es/gen_glue.py`, now with the profile's environment variable as an argument): `QUAKE3_GL_PROFILE`.
- **Screen modes:** the glimp fills `r_availableModes` (q3_ui reads it) with every 32-bit RTG mode of 640x480 or more, so the System menu offers 1920x1080, 2560x1440 and the like (set through `r_mode -1`); both Quake builds.
- **1920x1080:** 17.7 fps with renderergl2's defaults, the GPU 43 ms a frame on the per-pixel lighting shaders (normal and specular mapping, for maps Quake III's data does not have); with both off 44.8 fps (render 9.7 ms). `ports/ioq3-gl2/patches/0001-lighting-maps-off.patch` makes off their default (the menu can turn them on); 640x480 now 80.7 fps. The depth pre-pass costs nothing measurable.
- **Shipped shader cache:** the screen size is in every program's text, so each mode has its own 109 programs. Package m6-5 ships them for 640x480 and 1920x1080 (compiled on the A1200: 218 `.v3dp` and 432 `.v3ds` shader records, 4.4 MB, `tests/a1200/m6-5/cache`): a fresh install loads them in 2.4 s (107 s compiling before; the records save the compiler's front end, 40 ms a shader). Another mode compiles once, about 90 s, with "Compiling shaders for this screen mode, once: N of about 109" and a progress bar on the screen (patch `0002-shader-progress` and the glimp; the screen stayed grey before).
- **Gamma:** without a hardware gamma ramp the renderer applies `r_gamma` to the textures as they load (as the MiniGL build), so the menu's brightness works after its restart.
- **The Quake III test is now m6-5** (renderergl2, 80.7 fps at 640x480); m6-2 (gl4es) stays as the gl4es path's test.
- **Sources from a fresh clone:** the AmigaOS 3 engine (Cowcat's port, which is not on GitHub, with the PiStorm changes up to build 296) is published as `github.com/ladislawv/ioq3-amiga`; `ports/ioq3/sources.sh` clones it and upstream ioquake3 at pinned commits into `~` on the first build (`IOQ3_SRC`/`IOQ3_UP_SRC` name other trees). `CLAUDE.md` and `CONTRIBUTING.md` describe the setup for a new developer or agent.
- **A1200 runs:** `ogles2-m6-5-20261012-194531` (the split), `-200429` (640x480 80.7 fps), `-201443` (the shipped cache: 2.35 s, nothing compiled); the 1080p runs in the session's scratch results (44.8 fps).

**Done (2026-10-13): Quake III played; a second build machine.**
- The user's playing session on renderergl2 (menus, settings, a match) is complete.
- A second build machine (WSL2, set up from `CONTRIBUTING.md`) builds everything, including the ports; its `v3dglslc` matches the PC's byte for byte (`compare_v3dp.sh` 5/5). The setup scripts gained the CyberGraphX and AHI SDKs (`dev/host-setup`). With that machine's library and Quake build: `ogles2-m2-4-20261012-234543` every test passes; `ogles2-m6-5-20261013-001034` timedemo `four` **80.5 fps** (frame min/avg/max 5.0/12.4/82.0 ms), 0 dropped, 0 bytes of GPU memory left.

**Done (2026-10-13): every extension of Appendix B.3; ogles2.library 1.2.**
- **Library only:** `OES_texture_npot` (the library already behaved so), `EXT_read_format_bgra` (`glReadPixels` as BGRA bytes and the two reversed 16-bit packings), `EXT_texture_filter_anisotropic` (2 to 16), `EXT_texture_lod_bias` (a texture parameter, as AmigaOS 4's: there is no texture environment; s8.8, ±15; the sampler record's bias half-word was never swapped to little-endian, harmless while it was always 0), `SGIS_texture_lod` (base and max level in the texture state, min and max LOD in the sampler; the GPU copy is laid out from level 0, so level 0 must exist), `ARB_texture_mirror_clamp_to_edge` (in OS4's list, missing from B.3 before; `MIRRORONCE`), `OES_standard_derivatives` (Mesa compiles them; `dFdy` on the flipped default framebuffer reads the `FbWposYTransform` state variable the runtime already computes), `OES_get_program_binary` (the binary is the `.v3dp` file, `GL_PROGRAM_BINARY_V3DP_PISTORM`; images keep their file's bytes), and OS4's `glPolygonMode` (`GL_FRONT_AND_BACK` only: CFG_BITS' wireframe and point-fill modes; the fragment variant follows the class the rasteriser sees, as Mesa's), `glProvokingVertex` (state and CFG_BITS only: GLSL ES 1.00 has no flat varyings) and `glGetBufferParameterivOES` (an `alias` line in `gen/api.txt`: gl.xml has no such command). 159 dispatch entries.
- **Float and depth textures** (`OES_texture_float`, `OES_texture_half_float(_linear)`, `OES_depth_texture`, and `OES_packed_depth_stencil`'s `DEPTH_STENCIL_OES` texture): each level's RAM copy has a class (RGBA8, RGBA floats, or the stored depth word); storage after Mesa's format table (R/RG/RGBA 32F and 16F, `DEPTH_COMP16`, `DEPTH24_X8`, depth sampled as luminance). 32-bit floats and depth cannot be filtered: sampled nearest (logged). Depth textures are framebuffer attachments: the tile buffer stores into the level's own layout as D16 or D24S8, which the TMU samples.
- **The 32-bit TMU return** (Mesa's `sampler_is_32b` key) is a second file per program with samplers, `<hash>.v3dw` (`v3dp/FORMAT.md`): the same draws with every sampler bound to an RGBA32F texture. The runtime takes its fragment variants for a draw whose program samples a float or depth texture (the 32-bit return reads 8-bit textures correctly too, as Mesa's `V3D_DEBUG=tmu32` assumes), loaded from `PROGDIR:` or the cache, or compiled then by the on-target compiler (`V3DGLSL_LINK32`, about a second, once per program). Compiling them eagerly would have cost three more fragment compiles for every textured program (Quake III: 109). The offline compiler writes both files.
- **`ARB_texture_rectangle`:** Mesa patch 0011 (`tools/v3dglslc/patches`; apply it in `~/mesa-build/mesa` on an existing build machine, then `replay.py`, `build_m68k.sh`, `build_host.sh`) lets GLSL ES take `sampler2DRect` with `sampler2D`'s default precision, and the key-state draws bind rectangle textures; V3D 4.2's TMU samples unnormalized coordinates itself (the compiler's P1 bit), so no `TEXRECT_SCALE` uniform. Rectangle textures: level 0, `NEAREST`/`LINEAR`, clamped, also as render targets.
- **Tests:** `m6_ext` (88 checks) in package `m6-6`, whose step 5 runs it without binaries so the A1200 compiles every program and its `.v3dw` (87 checks, without the memory check: the compiler starts in it). `m2_texture` now tests an invalid type with `GL_INT` (`GL_FLOAT` is valid).
- **A1200 runs:** `ogles2-m6-6-20261013-105143` (`m6_ext` 88/88, every earlier test as before) and `-105228` (step 5: 87/87; all nine binaries compiled on the A1200, `.v3dw` included, byte-identical to the PC's; m2_link 27/27 once a stale cache file from a run with an old compiler was gone); `ogles2-m2-5-20261013-105342` the corpus 14/14; `ogles2-m6-5-20261013-105533` Quake III 80.6 fps; 0 bytes of GPU memory left everywhere. `compare_v3dp.sh` 7/7 (with the `.v3dw` files), `scan_asm.sh` 0 suspect functions.

**Done (2026-10-13): glmark2-es2 runs (an M6 exit criterion).**
- **Port:** `ports/glmark2` (README there). glmark2 2023.01 (pinned commit `22c527c`) needs C++17 with `std::filesystem`, so it is built with GCC 16.2 and libnix (`-mcpu=68020 -m68881`); `libogles2.a` from the GCC 6.5 build links unchanged (both write Amiga hunk objects, and the import library is plain C). Two new backends: `NativeStateAmiga` (the ogles2 context and its window) and `GLStateOGLES2` (glad over `aglGetProcAddress`). GCC 16.2 keeps `fintrz`, so no `-fbbb` is needed there.
- **Traps found in GCC 16.2's libstdc++ and libnix** (the port works around each; any C++ port with this toolchain meets them):
  - libnix starts every program in the system locale (decimal comma here), and `setlocale(LC_ALL, "C")` does not undo it, so glmark2's shader constants came out as `1,0`. The port defines a C-only `setlocale`/`localeconv`/`__locale_mb_cur_max`, so libnix's `setlocale.o` is never linked.
  - `std::filesystem::directory_iterator` returns nothing, with no error. The port uses `opendir`/`readdir`.
  - `std::chrono::steady_clock::now()` is `time()`: whole seconds (each scene moved once a second). libnix's `clock_gettime` and `gettimeofday` tick at 50 Hz. The port reads timer.device's E-clock.
  - libnix's headers declare `getrusage` and `statvfs` without `extern "C"`, and libstdc++ needs `__xpg_strerror_r`.
  - An AmigaOS 3 Shell cannot redirect stderr, so the port's errors go to stdout.
- **Result:** 800x600 window, default list at 10 s a scene: **score 253**, 32 of 33 scenes, 0 frames dropped in 94,230, 0 bytes of GPU memory left. `terrain` is "Unsupported" (it needs vertex texture fetch; ogles2 has 0 vertex texture units).
- **Slow scenes:** `refract` 3 fps (`glGenerateMipmap` on a 1600x1200 render target every frame: CPU readback, box filter and re-upload, about 500 ms; the TFU is the fix) and `buffer` 49–83 fps (half of a 200-column buffer rewritten every frame).
- **Not explained:** the A1200 froze twice during runs before the fixes (in `effect2d`'s set-up with the listing broken, and once in `shadow`). It did not recur in three full runs after them, and `shadow` alone for 10 s is steady (122 fps, constant GPU memory).
- **Test:** package `m6-7` (`Run-M1` keys `DURATION`, `SIZE`, `BENCH`, `DEBUG`, `PROBE`).
- **A1200 runs:** `ogles2-m6-7-20261013-122715` (E-clock, score 253) and `-122144` (seconds clock, with the periodic probe). Diagnosis: `-114113` (the decimal comma), `-114757` (errors on stdout), `-120850` and `-120914` (debug, the listing fixed), `-121925` (`refract` alone: 501 ms of CPU work a frame) and `-122111` (`shadow` alone).

**Done (2026-10-13): `glGenerateMipmap` with the TFU.**
- **v3dcore:** `hw/v3d_tfu.c`, `include/v3d_tfu.h`: the TFU registers (hub `0x400`–`0x434`, checked against Linux `drivers/gpu/drm/v3d/v3d_regs.h`), a mipmap job with Mesa's words (`v3dx_tfu.c`, V3D 4.2, `for_mipmap`: source and destination level 0, `IOA` `DIMTW`, `ICFG` `NUMMM`, `OPAD` for a UIF level 0), written in the kernel's order with `ICFG` last. Completion is polled on `TFU_CS`'s conversion count, as Mesa's simulator does; a job that does not finish resets the TFU and the CPU makes the levels.
- **Library:** §7.6. Before the job: the job that draws level 0 renders first, one that samples the texture and has not rendered yet finishes, the CPU data cache is cleaned; the job runs under the GPU submission lock. The texture cache is invalidated at the next render job's start, as for any upload. Probe `M` keeps the CPU path.
- **Test:** `tests/m6_tfu` (13 checks: every level of a 256x256 noise texture and a 200x120 gradient against a 2x2 box filter, a 512x512 render target's quadrants per level, RGB565 and RGBA4), package `m6-8`, which runs it on the CPU (probe `M`) and on the TFU, then `m2_texture` and `m5_fbo`.
- **A1200 runs:** `ogles2-m6-8-20261013-125212`: 13/13 on both paths (the TFU's levels within 2 of the box filter: its rounding), `m2_texture` 33/33, `m5_fbo` 44/44. Draw into a 512x512 target and `glGenerateMipmap`: 46.2 ms a frame on the CPU, 1.18 ms with the TFU. glmark2 `refract` 3 → 48 fps (`ogles2-m6-7-20261013-125255`); the whole list `ogles2-m6-7-20261013-125332`: score 251, 0 dropped of 94,421 frames.

**Done (2026-10-13): texture uploads, with the TFU.**
- **Measured first** (`m6_tfu`'s upload timings and a temporary per-step log): a 1024x1024 RGBA8 `glTexSubImage2D` took 61 ms, of which the conversion into the RAM copy was 45-52 ms (a switch per texel), the CPU's tiling 5.6 ms; a 16-bit format's packing cost as much again and its tiling about 40 ms a megapixel.
- **Changed** (§7.6): per-format conversion and packing loops; the TFU tiles large whole levels (`v3d_tfu_store_raster` in `v3dcore/hw/v3d_tfu.c`, sharing the mipmap job's submission; the CPU cache cleaned before, the level's lines invalidated after, so a later partial store by the CPU reads no stale line). Probe `U`.
- **Result** (A1200, `ogles2-m6-8-20261013-131333`): 1024x1024 RGBA8 `glTexImage2D` 51-58 ms → 9.2 ms, `glTexSubImage2D` 61 → 8.9 ms; 1024x1024 RGB565 138-144 → 24 ms (60 on the CPU path); 256x256 RGBA8 3.5 → 0.7 ms. `m6_tfu` 22/22 on both paths (TFU uploads drawn back exactly, also under a later partial store by the CPU), `m2_texture` 33/33, `m5_fbo` 44/44. The whole earlier suite `ogles2-m6-6-20261013-131354`: every test passes (step 5 compiling on the A1200 included). Quake III `ogles2-m6-5-20261013-131525`: 81 fps, its texture uploads 958 → 442 ms.

**Done (2026-10-13): vsync at the display's refresh (§7.2).**
- Investigation and findings in §7.2 (`tests/m6_flip/m6_flip.c` without GL: tearing, `WaitTOF`, `WaitBOVP`, `ChangeScreenBuffer`, panning, and the CPU each leaves idle; `m6_vsync.c` with ogles2; package `m6-9`, runs `ogles2-m6-9-20261013-131833` to `-143604`). Two and three screen buffers were built and measured, then dropped: on this RTG `ChangeScreenBuffer` costs a whole refresh per call.
- `display.c`: `ogles2_display_wait_refresh` (`WaitBOVP` on a screen of its own, else `WaitTOF`, at priority 10) before the swap's flush (`frame.c`), `WaitTOF` before a window's copy; `aglSetParams2`'s `VSYNC` switches it at once.
- Quake III (`ports/ioq3-gl2`): `r_swapInterval` (latched, as upstream's) asks for vsync; `Run-M1` and `Play-Quake` in `m6-5` take `VSYNC=1`, `Run-M1` also `TIMEDEMO=0` (the demo at its own speed, paced as a game is) and `MAXFPS`. 60 fps steady in the demo at its own speed (`ogles2-m6-5-20261013-143750`).

**Done (2026-10-13): 4× MSAA on the default framebuffer (§7.6's MSAA note).**
- `OGLES2_CCT_PISTORM_MSAA` (`include/libraries/ogles2.h`), `ENV:OGLES2/MSAA`; v3dcore's `g_v3d_requested_msaa`/`V3DContext.msaa` and `v3d_frame_compute_pool_sizes`'s tile size; `job.c` (tiles, configuration packets, decimate modes, depth across a flush), `draw.c` (`CFG_BITS` oversample, `SAMPLE_STATE` mask), `GL_SAMPLE_BUFFERS`/`GL_SAMPLES`. `glSampleCoverage` now bumps the state version (it changed nothing before MSAA).
- **Test:** `tests/m6_msaa` (9 checks: the queries, antialiased edges, coverage and its inverse, depth across a mid-frame flush, a framebuffer object in an MSAA context), package `m6-10`, which also runs `m1_draw`, `m2_texture` and `m5_fbo` as before and with `ENV:OGLES2/MSAA` 4.
- **A1200:** `ogles2-m6-10-20261013-135758`: every check passes (`m6_msaa` 9/9; the earlier tests the same with every context multisampled). Quake III (`m6-5`'s `MSAA=1`, `ogles2-m6-5-20261013-135929`): 81 fps, the same as without (the game is CPU-bound), the edges smoothed.

**Done (2026-10-13): vsync through SDL; frame-time targets (an M6 exit criterion).**
- SDL3 `testgles2 --vsync` in a window and full screen: 59.8 and 60.1 fps, no tearing or stutter (the developer watching); package `m6-4` steps 7 and 8 (`Run-M1 FROM=7`). Runs `ogles2-m6-4-20261013-145106` and `-145430`.
- §10.2: the frame-time targets, from this week's measurements, and the goals not met yet.
- M6's exit criteria: SDL (met), gl4es glxgears and Quake (met), glmark2-es2 (met), frame-time targets (met); a MiniGL game beside an ogles2 program is postponed with D3 (below).

**In progress (2026-10-13): M7, dEQP-GLES2 conformance (§9).**
- **The runner** (`ports/deqp`, its README): VK-GL-CTS `opengl-es-cts-3.2.14.1`'s `deqp-gles2`, built with GCC 16.2 and libnix by its own script (not the CTS's CMake), on an AmigaOS 3 platform: one ogles2 context in a 256x256 window, the GL functions from `aglGetProcAddress`, delibs' OS layer for one task (no threads; the E-clock). Traps beyond glmark2's: `int32_t` is `long` in this toolchain (dEQP takes it to be `int`: built with `__INT32_TYPE__` redefined); `amiga` is a predefined macro; three big-endian bugs in dEQP itself (patches: a 68k CPU for delibs, the ETC1 `.pkm` loader, the point-size check's pixel read); AmigaDOS has no `./` (`--deqp-archive-dir`).
- **Running it:** package `m7-1` (`Run-M1` keys `LIST`, `CASES`, `NAME`, `MSAA`, `IMAGES`, `TEST`), the must-pass list (17,165 cases) split by group as `caselists/`; from the PC `ports/deqp/run_groups.py` (group by group through `amictl`), `summary.py` (totals and what failed), `failed_list.py` (a list of the cases to run again), `qpa_images.py` (the logged images as PNG). The whole list took about 2.5 hours on the A1200, mostly the shader groups' compiles on the Amiga.
- **Results of the first full run:** non-shader groups 6,289 cases, shader groups 10,101: 9,900 passed. Every failure it found was fixed, except one deviation:
  - **Library fixes:** the blend constant colour (its packet's field order was reversed: 492 blend cases); deleted textures and renderbuffers attached to a framebuffer that is not bound live on without their name (GLES 2.0 4.4.3; 45 fbo cases); BGRA render targets and renderbuffers (`EXT_texture_format_BGRA8888`); `GL_SUBPIXEL_BITS`, the aliased line-width and point-size ranges, stencil masks as unsigned floats; polygon offset units for a 16-bit depth buffer; a depth-only or stencil-only clear no longer changes the other's pending clear value; sampler arrays (a texture unit per element); completeness with mismatched level formats; `glCopyTexSubImage2D`'s and `glGenerateMipmap`'s errors; `glGetVertexAttribfv` of the array state; 16 vertex texture units (0 until now); name tables for names a program never generated (a texture named 70,000,000 took a 512 MB table); register spilling (a scratch area per context: dynamically indexed arrays); attribute aliasing (two names bound to one location: the program is linked with its bindings, `V3DGLSL_LINK_BOUND`); a matrix attribute whose columns are not all read (the binary says which, FORMAT.md).
  - **Compiler fixes:** GLSL's `gl_MaxDrawBuffers` and `gl_Max*TextureImageUnits` as ogles2 reports them (1 and 16; Mesa's v3d says 4 and 24), the uniform-vector limits the other way round (4088 vertex, 4095 fragment); `gl_PointSize` clamped to 512 (Mesa patch 0012).
  - **The deviation:** V3D always filters cube maps seamlessly (across the faces' edges, as GLES3 requires); dEQP-GLES2 expects that only of a context that also answers GLES3's `GL_MAJOR_VERSION` (Mesa's v3d is a GLES 3.1 driver), and of a GLES2-only one each face clamped alone. 25 cube cases (`texture.mipmap.cube`, `texture.vertex.cube`, `shaders.texture_functions.vertex.texturecubelod`) fail by design; the developer's decision (2026-10-13): documented, not emulated.
  - **Not supported, by design:** several colour attachments (`EXT_draw_buffers`), clip control, debug markers, a compressed format with sub-image updates (ETC1 has none), more than 16 combined texture units (8 cases). All five are being added (`docs/M7_EXTENSIONS_PLAN.md`; steps 1-4 done, below).
- **Next:** the whole list again on the final library and compiler (a regression check of every change above), the earlier test packages and Quake III (its shipped shader cache compiled again: the compiler changed).

**Done (2026-10-13): M7 extensions, steps 1-4 of `docs/M7_EXTENSIONS_PLAN.md`; ogles2.library 1.3.** Details and runs in that plan, each step's section.
- **Compiler id:** a build stamp compiled into `v3dglslc` and hashed with its size (`ogles2_compiler_id`), so two builds of one size no longer share an id.
- **`GL_EXT_debug_marker`** (`ogles2/debug.c`): markers in the log with probe `T`, indented by group depth. The dEQP port's platform had never loaded extension functions (only `glw::initES20`): `debug_marker.random` called a null pointer; it now calls `glu::initExtensionFunctions` too.
- **32 combined texture units** (16 a stage): `OGLES2_MAX_TEXTURE_UNITS` 32, `OGLES2_MAX_STAGE_SAMPLERS` 16; the compiler's `MaxCombinedTextureImageUnits` 32.
- **S3TC** (`EXT_texture_compression_dxt1`, `_s3tc`, `ANGLE_texture_compression_dxt3`/`_dxt5`): kept compressed, sampled by the TMU as BC1-BC3; v3dcore lays levels out in 4x4 blocks (`v3d_texture_alloc_blocks`); `glCompressedTexSubImage2D` on block bounds. Test `m7_s3tc`, package `m7-2`.
- **`GL_EXT_clip_control`:** the probe showed V3D 4.2's window-space Z planes clamp, so `ZERO_TO_ONE` is done in the vertex shader: Mesa patch 0013 (`QUNIFORM_CLIP_Z_COEFF`, z becomes 2z - w; one uniform and three instructions in every vertex and binning shader). Upper-left origin by the viewport's Y scale; the clipper's Y scale now follows the shader's (it was always negative, wrong for framebuffer objects' clipped primitives). Test `m7_clip`, package `m7-3`. 163 dispatch entries.
- **Step 5, `GL_EXT_draw_buffers` (2026-10-14)**, with `NV_fbo_color_attachments` and `OES_required_internalformat`: four colour attachments, `glDrawBuffersEXT` (164 dispatch entries), a job with 1-4 render targets (Mesa's tile sizes, a load/store/clear per target), partial clears through a program in the library's data (`tools/clear_mrt`), compiler variants for 2-4 targets for programs that write `gl_FragData[1]` or up. Deviation: a `gl_FragColor` program draws into the first target only (the extension broadcasts it). Test `m7_mrt`, package `m7-4`. dEQP `fbo` 709/709 (the 54 `attachment_combinations` cases now run).
- **dEQP:** `debug_marker`, `uniform_api`, `negative_api`, `implementation_limits`, `state_query`, `shaders.builtin_variable`, `clip_control`, `clipping`, `depth_range`, `polygon_offset`, `fbo` all pass; `texture` 783/807 (the 24 cube cases of the deviation). For `clip_control`, the dEQP port's patch 0004 (its cases bind vertex array objects unchecked) and `glTexImage2D` taking `GL_RGBA8_OES`/`GL_RGB8_OES` (as `OES_required_internalformat`).

**Done (2026-10-14): the final M7 conformance and regression run** (library 1.3, compiler with Mesa patches 0001-0013, every package cache emptied first).
- **dEQP-GLES2, the whole must-pass list (17,165 cases):** 17,130 passed, 27 failed, 6 warnings (`flush_finish` timing 3, `rasterization.interpolation` quality 3), 2 not supported (`multisampled_render_to_texture`: no `EXT_multisampled_render_to_texture`; one shader case). The failures: the 25 cube-map cases of the documented deviation (§13 M7 entry; seamless filtering), and with `multisample` run in a 4x MSAA context (`Run-M1 MSAA=1`), `proportionality_alpha_to_coverage` (alpha to coverage not implemented) and `stencil` (to be looked at); both fixed on 2026-10-15 (below). Runs `ogles2-m7-1-20261014-181436` to `-191934`, `-192743` (`shaders-random`), `-operator`, `ogles2-m7-1-20261015-000040` (MSAA).
- **The earlier packages and the new ones:** `m5-1`, the corpus (14 of 14 against the references), `m6-6` (88/88, and 87/87 compiled on the A1200), `m6-8`, `m6-10`, `m6-4` (SDL; vsync 60.0 fps), `m6-7` (glmark2 score 242, `terrain` 26 fps with vertex texture fetch), `m7-2`, `m7-3`, `m7-4`: every check passes.
- **Quake III:** its shipped cache compiled again (228 programs, 452 shader records; the compiler changed), a fresh install compiles nothing; timedemo `four` **75.4 fps** (13.3 ms average: within §10.2's 13.5 ms, but 7% slower than the 80.5-81 fps of 2026-10-12/13).
- **Found on the way:**
  - **`amictld`'s large-reply stall is real.** After `shaders-random` (a 2.7 MB log), the server stopped answering, and only a reset brought it back. The run had finished.
  - **A slowdown since 2026-10-13:** besides Quake, dEQP's `shaders-operator` took about 2 hours against 90 minutes. Suspects: the clip-control coefficient in every vertex and binning shader (patch 0013), and the draw-buffer variant choice per draw.

**Done (2026-10-15): the two 4x MSAA failures.**
- **`multisample.stencil`:** depth and stencil were stored across a mid-frame flush only when the job had drawn. A job with nothing but a colour clear between two read-backs dropped them, and the next job loaded a cleared stencil. `job.c` now also stores them when the job loaded them or cleared them. Test: `m6_msaa`'s new "stencil across two flushes" check (10 checks).
- **Alpha to coverage:** V3D has no fixed-function alpha to coverage; Mesa does it in the fragment shader (`SETMSF` with the coverage from alpha) when the key has `msaa` and `sample_alpha_to_coverage`. Mesa patch 0014 (`v3d_force_alpha_to_coverage`) sets both while `v3dglslc` draws a program's key states, which it writes as a third file, `<hash>.v3da` (request `V3DGLSL_LINK_A2C`, `v3dp/v3dglsl_port.h`). The library loads it like the `.v3dw` (`PROGDIR:`, the cache, else the on-target compiler, once per program; about 100 ms) and uses its fragment variants for draws into a 4x MSAA default framebuffer with `GL_SAMPLE_ALPHA_TO_COVERAGE`. Not together with 32-bit samplers: such a draw keeps its `.v3dw` variants and covers every sample.
- **Compiler:** host and m68k builds with patches 0001-0014; `compare_v3dp.sh` 8/8. Plain and 32-bit variants are unchanged, so Quake III's shipped cache stays valid (accepted-shader records do not depend on the compiler id).
- **A1200:** `ogles2-m6-10-20261015-001430` (`m6_msaa` 10/10, the earlier tests as before, with and without MSAA); dEQP `multisample` with `MSAA=1` 16/16 (`ogles2-m7-1-20261015-001505`); Quake III timedemo 75.7 fps from a fresh install, nothing compiled (`ogles2-m6-5-20261015-001640`).

**Done (2026-10-15): Quake III's slowdown measured.** Timedemo `four`, 640x480, every run with a fresh `q3config.cfg` and cache, each library with a Quake linked against its own `libogles2.a` (the client checks the dispatch table's size). Libraries built from each commit's sources, the old RTG driver back in (a new one had been installed since 2026-10-12; it made no difference that mattered: the reference build 79.2-79.3 fps with either).
- **Where:** `c252b50` (MSAA, the 81 fps of 2026-10-13) 79.2, 79.2; `c18e5de` (vsync) 79.5, 78.0-79.6 in later runs; **`bb8665d` (the dEQP fixes) 76.6-77.0**, the same with its own cache and with the one compiled again (`2d05366`): the library, not the shaders. After it nothing moves beyond the noise (about ±1 fps between runs of one build): `2d05366` 77.0-77.1, steps 1-4 (`6004dca`) 76.4-78.7 (77.1 on average over three), step 5 (`51916dd`) 76.4-77.5 (77.0 over three), `83f7503` 75.1-78.8. Mesa patch 0013's shader work: none measurable (the current library with the cache from before it, 78.6 and 78.8, against 78.5 and 78.7).
- **What:** inside the draws, the uniform streams (timers around each, a temporary build): the same work before and after `bb8665d` (120,710 vertex streams reused, 192,740 written, in both), each 20-40% slower (vertex 1.8 to 2.5 us a draw). `bb8665d` added two cases to `write_uniforms`' switch, and GCC compiled the function differently (336 to 393 instructions, a value kept in the frame, not a register, in its loops). A rewrite that does less (the constants of a stream in a template made at load, the switch in a function of its own) made the uniform streams 11% faster, but the other parts of the draw grew by as much and the frame rate stayed: reverted. 1 KB of padding before `write_uniforms` changed nothing; `-Os` cost 12% (67 fps). On Emu68 the cost follows how GCC compiles the draw path more than the work it does; winning back the 3% would mean trying `bb8665d`'s per-draw additions (sampler arrays an element at a time, the spill area, attribute columns) one by one.
- **Not explained:** the reference build's 79.2-79.6 against its 81 two days earlier. Its runs here used its era's cache plus 15 programs compiled by today's compiler. MiniGL's build has not changed (below).
- **Against MiniGL** (the installed `quake3`, minigl.library 29.1, the same settings, its home in `RAM:`): `r_primitives 0` **61.3 and 61.5 fps** (16.3 ms; 60.8 on 2026-10-12), its default `r_primitives 1` 34.0. ogles2's build (renderergl2, lit per pixel) 75.1-76.9 (13.0-13.3 ms): **about 24% faster**, its worst frames slower (70-81 ms against 37-39).
- **Decision:** the frame stays within §10.2's 13.5 ms; the slowdown is understood, not chased further for now.
- **A1200 runs:** `ogles2-m6-5-20261015-113623` to `-125237`.

**Done (2026-10-15): the licences (D6, §12).**
- A file-by-file check of the front end: six files say they adapt PiStorm3D's code (`job.c` from `gl_FrameBegin`/`gl_FramePresent`, `display.c` from its `vid_*` functions, `draw.c` its dedup cache and clear rectangle, `program.c` the built-in image's record values, `texture.c` its state packing, `builtin_shaders.h` its shaders); they and `v3dcore/` are under the Hyperion terms, each saying so in its opening comment. The rest of `ogles2/`, `client/`, the public headers, `cmd/`, `tests/` and the tools are MIT. Nothing outside `v3dcore/` and `tools/builtin_shaders/` includes the backend.
- `LICENSE` lists the files under each licence and gives the ports' licences (ioquake3 GPL v2+, glmark2 GPL v3, gl4es MIT, SDL zlib, VK-GL-CTS Apache-2.0) and what distributing their binaries requires; `README.md` and `CONTRIBUTING.md` (new code MIT; code adapted from PiStorm3D or MiniGL moves its file under Hyperion and into `LICENSE`) agree with it.
- Before: `client/` and the headers were listed under Hyperion, whose AmigaOS-only clause a GPL program may not take on; the Quake III and glmark2 binaries linked them.

**Done (2026-10-15): the release archive, SDK and installer (M7).**
- `release/build_release.sh` (in WSL) builds `~/ogles2-build/release/ogles2-1.3.lha` (5.1 MB; 19.8 MB unpacked, 19 of them the compiler): `Libs/` (`ogles2.library`, `v3dglslc`), `C/OGLES2Log`, `Examples/` (two programs with their `.v3dp`), `SDK/` (`include/` with the generated `proto/ogles2.h` and `libraries/ogles2_dispatch.h`, `lib/libogles2.a` and `lib/ogles2.lib` for vbcc, `doc/ogles2.doc` the autodoc, `examples/` sources and Makefile), `ReadMe`, `Install-ogles2`, `LICENSE`, `licences/` (with Mesa's notices for the compiler: none of the Mesa files it is built from is other than MIT).
- **The examples** (`release/examples/`): `triangle` (a buffer object, a uniform, the program's own window with a close gadget through `OGLES2_CCT_WINDOW`) and `cube` (index buffer, `glGenerateMipmap`'d texture, depth buffer, matrix uniform, a resizable window: at `IDCMP_NEWSIZE` it passes the window again with `aglSetParams2` and sets the viewport). Their shaders' binaries are compiled by the host `v3dglslc` from the exact source bytes (`release/emit_shaders.c`) and ship beside them. The builder compiles them from the assembled SDK with its own Makefile, with GCC and with vbcc, so the SDK is what is tested.
- **The autodoc** (`release/ogles2.doc`): the `agl*` calls and `OGLES2Open/Close`, the binary lookup order and the compiler, the extensions, limits, environment variables, and the deviations: seamless cube maps, `gl_FragColor` into the first draw buffer only, `glPolygonMode` both faces, no multisampled framebuffer objects, float textures nearest, the tags accepted and ignored (`RESIZE_VIEWPORT`, `SHADER_COMPAT_PATCH`, `SINGLE_GET_ERROR_MODE`, `DETECT_UNINITIALIZED_GLSL_VARS`, the SPIR-V ones), the tag values not OS4's.
- **The archives in the repository** (`release/dist/`, 2026-10-15, for development on Windows): `ogles2-1.3.lha` (users: library, compiler, installer, examples), `ogles2-1.3-sdk.lha` (the same with `SDK/`), `ogles2-1.3-sdk.zip` (`SDK/` alone with the licences) and `SHA256SUMS`; `build_release.sh dist` refreshes them.
- **The programmer's guide** (`release/GettingStarted.txt`, in the archive as `SDK/doc/GettingStarted.txt`, 2026-10-15): a first program, the build flags and why (`-fbbb`, `-lm881`, `-fno-builtin`), windows, full screen (input through `OGLES2_CCT_PISTORM_GET_WINDOW` and `ModifyIDCMP`: ogles2's own windows have no IDCMP), resizing, shaders and how to ship their binaries, performance, shipping (with the library's licence), a symptom-to-cause table, porting from OS4/EGL/desktop GL, and a prompt with rules for an AI assistant plus follow-up prompts and a checklist for its output.
- **The installer** (`release/Install-ogles2`, an AmigaDOS script; Installer is not on every 3.x system): asks for each of `LIBS:ogles2.library`, `LIBS:v3dglslc`, `SYS:OGLES2Cache` with `Assign OGLES2Cache:` added to `S:User-Startup` between `;BEGIN ogles2` and `;END ogles2`, and `C:OGLES2Log`; `ALL` does all four without asking (the test station cannot answer `Ask`: its commands' input is empty, which answers no).
- **Without installing:** `OGLES2Open()` and the compiler look in the program's drawer (`PROGDIR:libs/`, then `PROGDIR:`) when `LIBS:` has none (§5, §8.4). On the A1200, with neither installed and the current directory `RAM:`: the examples from a drawer with `libs/ogles2.library` and their `.v3dp` ran with no compiler; from a drawer with `ogles2.library` beside them, `libs/v3dglslc` and no `.v3dp`, the compiler started from there (1.2 s for the first shader, 0.24-0.32 s per program) and wrote into `T:` the same bytes as the shipped `.v3dp`. Both draw correctly (screenshots).
- **Installed on the A1200 (2026-10-15)** from the archive unpacked there with its LhA: without `ALL` every step skipped and nothing changed; with `ALL` the four parts installed and the assign added; run again, the assign was not added twice. Then `cube`: from the archive's drawer (its `.v3dp`), resized twice while running (`ChangeWindowBox` from a helper) to 632x447 and 212x127, drawn to the new size with the right aspect each time; a copy without its `.v3dp` compiled by `LIBS:v3dglslc` into `OGLES2Cache:` (1.4 s, the compiler's start included); run again, loaded from there without the compiler. Found on the way: with the compiler installed, a program's first `glCompileShader` starts it even when the program ships its binary (the real compile status, §7.8), about 1.2 s once; the shaders' records in the cache spare it from then on. Then uninstalled again (`S:User-Startup` restored byte for byte): an installed library would override every test package's (CONTRIBUTING.md, Traps).
- **A program's newer copy against an older installed one (2026-10-15):** with 1.2 (`build-2d05366`, a smaller dispatch table) in `LIBS:` and 1.3 in the program's `libs/`: unused, the 1.2 was loaded, refused, expunged, and the program ran on its 1.3 (both `init` lines in the log); held open by another program, the program stopped with its message (rc 20) and the other was not disturbed.

**Done (2026-10-16): the SDL archive.** `release/build_release.sh sdl` builds `ogles2-1.3-sdl.lha` (3.4 MB) and `ogles2-1.3-sdl.zip` (2.3 MB), which unpack over the SDK archive: `SDK/SDL2/` and `SDK/SDL3/` (headers, `libSDL2.a`/`libSDL3.a` and the test libraries, built by `ports/sdl2` and `ports/sdl3`), `SDK/MiniGL/` (PiStorm3D's minigl.library 29.1 client SDK: SDL links it for its desktop-GL path and `SDL_opengl.h` includes it; Hyperion licence, which allows it on AmigaOS), `SDK/doc/SDL.md` (`docs/SDL.md`: what the ports add, the figures, how to build and use them), `SDK/examples-sdl/` (`release/examples-sdl/`: `sdl2_sprites`, 64 rotated, blended sprites through SDL's GLES2 renderer; `sdl3_gles2`, its own shaders and buffer in an SDL3 ES 2.0 context; a Makefile), `Examples/SDL/` (both built from the assembled SDK and SDL archive), `ReadMe-SDL`, SDL's zlib licences and MiniGL's. On the A1200 both ran from a drawer with the release's library and compiler in `libs/` (vsync on, in windows on the 1920x1080 Workbench): `sdl3_gles2` 49.4 fps (a spinning triangle), `sdl2_sprites` 47.2 fps (the sprites rotating and blended), both drawn correctly (screenshots), the shaders compiled at the first run and loaded from the cache after.

**Next, in order (M6: ecosystem, sharing and performance, §9):**
1. **M7, before the release:**
   - `amictld`'s large-reply stall (postponed: the test station, not the library);
   - the repositories made public with the release (the Hyperion terms); the backend's authors asked whether their code may become MIT (D6).
2. **The rest of M6's scope:** optional: native ETC1 sampling. glmark2's `buffer` scenes (49–83 fps: half of a 200-column buffer rewritten every frame) are a candidate for the buffer path.
3. **Postponed (2026-10-13, the developer's decision):** per-draw CPU time for gl4es programs (the gl4es build: 23.3 ms a frame against MiniGL's 16.4; ogles2's draw about 28 us, 32 us for renderergl2's), and minigl.library coexistence (D3: the shared submission component with PiStorm3D's maintainers; the turn across programs above is ogles2's half).

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
| `SHARE_WITH` | M6, done: textures, buffers, shaders and programs, renderbuffers shared with the given context's group (`share.c`); framebuffers and bindings stay per context; the group's contexts may be used from several tasks (each call under the group's semaphore); a deleted object is detached in every member; an invalid context: `OGLES2_CCE_BAD_TAGS` |

PiStorm-specific tags (for example MSAA, swap mode) go in a private value range.

### B.3 Extensions

| OS4 ogles2 exposes | Plan |
|---|---|
| `GL_OES_mapbuffer` (`glMapBufferOES`, `glUnmapBufferOES`, `glGetBufferPointervOES`) | M4; done |
| `GL_OES_element_index_uint` | M4; done |
| `GL_OES_get_program_binary` (`glProgramBinaryOES`, `glGetProgramBinaryOES`) | M6; done (the binary is the `.v3dp` file, format `GL_PROGRAM_BINARY_V3DP_PISTORM`) |
| `GL_OES_packed_depth_stencil` | M5; done |
| `GL_EXT_texture_format_BGRA8888` | M4; done |
| `GL_EXT_blend_minmax` | M4 (native blend modes); done |
| `GL_OES_texture_float` | M6; done (the 32-bit TMU return key: the program's `.v3dw` variants; sampled nearest), with `OES_texture_half_float` and `_half_float_linear` |
| `GL_EXT_texture_filter_anisotropic` | M6; done (2, 4, 8, 16) |
| `GL_EXT_texture_lod_bias`, `GL_SGIS_texture_lod` | M6; done (the bias a texture parameter, as OS4's; base and max level, min and max LOD) |
| `GL_ARB_texture_rectangle` | M6; done (Mesa patch 0011 for GLSL ES; V3D 4.2 samples unnormalized coordinates itself, no `TEXRECT_SCALE`) |
| `GL_ARB_texture_mirror_clamp_to_edge` (in OS4's list, missing here before) | M6; done |
| `glDrawElementsBaseVertexOES` (`OES_draw_elements_base_vertex`) | M4; done |
| `glPolygonMode`, `glProvokingVertex`, `glGetBufferParameterivOES` (OS4 extras) | M6; done (CFG_BITS' Direct3D wireframe, point-fill and provoking-vertex bits; `GL_FRONT_AND_BACK` only, the hardware has one mode) |
| *Not in OS4 list, planned:* `OES_texture_npot`, `OES_compressed_ETC1_RGB8_texture` (done, M4), `EXT_read_format_bgra`, `EXT_discard_framebuffer` (done, M5), `OES_standard_derivatives` (Mesa's compiler supports derivatives; works in S1 and S2), `OES_depth_texture` | M4–M6; all done (M6: depth textures 16- and 24-bit, drawn into and sampled, and a `DEPTH_STENCIL_OES` one for both) |
| *Not in OS4 list, offered since M5:* `OES_rgb8_rgba8`, `OES_fbo_render_mipmap` (Mesa offers both) | M5; done |
| *GLES 3.0's* `glDrawRangeElements`, through `aglGetProcAddress` (ogles2 1.1): the indices' range given, not scanned (gl4es passes a locked array range) | M6; done |
| *Not in OS4 list, for dEQP-GLES2:* `EXT_debug_marker` (markers in the log with probe `T`) | M7; done (ogles2 1.3) |
| *Not in OS4 list:* `EXT_texture_compression_dxt1`, `EXT_texture_compression_s3tc`, `ANGLE_texture_compression_dxt3`, `ANGLE_texture_compression_dxt5` | M7; done (kept compressed: the TMU samples BC1-BC3; sub-image updates on 4x4 block bounds) |
| *Not in OS4 list, for dEQP-GLES2:* `EXT_clip_control` | M7; done (`ZERO_TO_ONE` in the vertex shader, Mesa patch 0013: V3D 4.2's Z planes clamp) |
| *Not in OS4 list, for dEQP-GLES2:* `EXT_draw_buffers`, `NV_fbo_color_attachments` | M7; done (4 render targets; a `gl_FragColor` program draws into the first only) |
| *Not in OS4 list:* `OES_required_internalformat` | M7; done (sized internal formats in `glTexImage2D` and `glCopyTexImage2D`) |

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
