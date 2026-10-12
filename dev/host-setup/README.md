# Host setup for ogles2 development

Scripts that set up the build machine described in §2.4 of [`docs/DEVELOPMENT_PLAN.md`](../../docs/DEVELOPMENT_PLAN.md). Everything runs in Ubuntu 24.04 (WSL on Windows, or native Linux), in the Linux filesystem (`~`). The repository itself may live anywhere, including a Windows drive under `/mnt/c`.

| Script | What it does |
|---|---|
| `build_tools.sh` | Installs LHa for UNIX (`~/tools/bin/lha`, the test packages' archiver) and amitools (`~/amitools-venv`, vamos for the host tests) at the commits used here. A few minutes. |
| `build_toolchains.sh` | Builds bebbo's amiga-gcc twice, every project at the commit in `toolchain-pins.txt` (`PINNED=0`: the branch heads, for a deliberate toolchain update): GCC 6.5.0b (`amiga6`, PiStorm3D's toolchain, with clib2 and amiga-gcc's CyberGraphX and AHI SDKs, `make sdk=cgx` and `sdk=ahi`) into `~/amiga-gcc-install`, and GCC 16.2.0b (`amiga16.2`, for the Mesa compiler port, with libnix and libstdc++) into `~/amiga-gcc162-install`. `TARGETS=gcc16` builds the older `amiga16.1` into `~/amiga-gcc16-install`, which miscompiles and is kept only for comparison. Logs and exact commits: `~/toolchain-build/summary.log`. |
| `clib2-ndk32.patch` | Lets adtools clib2 `master` (`c026659`) build against the NDK 3.2 headers amiga-gcc now ships: an invalid `##` paste in the constructor macros, `<time.h>`'s `tv_sec` macro clashing with NDK 3.2's `struct timeval`, and a missing `IntuitionBase` declaration. Applied to GCC 6 only; under GCC 16 clib2's inline asm no longer compiles. |
| `pthread-timersub.patch` | Adds the BSD `timersub` macro to amiga-gcc's libpthread. Without it GCC 6 leaves `_timersub` unresolved, and GCC 16 stops, which also skips libgcc and libstdc++. |
| `build_mesa.sh` | Builds Mesa at the pinned commit `74b6971` with the gallium `v3d` driver, the v3d drm-shim and llvmpipe into `~/mesa-install`. |
| `patch_drmshim.sh`, `drm-shim-wsl1.patch` | WSL1 has no `memfd_create`, which drm-shim needs. The patch falls back to an unlinked temporary file. Test tooling only. |
| `build_emu68.sh` | Builds Emu68 (default `v1.1.0-beta.1`, the test machine's version) for PiStorm into `~/emu68/`. Emu68 is big-endian AArch64: copy `stubs-lp64.h` to `stubs-lp64_be.h` in `/usr/aarch64-linux-gnu/include/gnu/` first, as Emu68's README says. Packages are listed in the script. |
| `build_vbcc.sh` | Installs vbcc 0.9h patch 2 (m68k-amigaos target), vasm 1.9 and vlink 0.17 into `~/vbcc` (checksums in the script), for the vbcc client test (`make vbcc`, plan §6.3). Uses the GCC 6 toolchain's NDK 3.2 headers, so it runs after `build_toolchains.sh`. vbcc is free for non-commercial use. |
| `shimtest.c`, `run_shimtest.sh` | Smoke test: compiles a GLES2 program on V3D 4.2 through the drm-shim (prints the QPU code with `V3D_DEBUG=qpu`) and draws it with llvmpipe. |

Package prerequisites (run once as root):

```
apt-get install make wget curl git gcc g++ lhasa libgmp-dev libmpfr-dev libmpc-dev flex bison gettext \
  texinfo libncurses-dev autoconf automake libtool rsync libreadline-dev python3-pip python3-venv \
  python3-mako python3-yaml python3-packaging ninja-build pkg-config libdrm-dev libexpat1-dev zlib1g-dev \
  libzstd-dev libelf-dev cmake llvm-dev libclang-cpp-dev libunwind-dev libsensors-dev \
  libx11-dev libxext-dev libxfixes-dev libxcb-glx0-dev libxcb-shm0-dev libx11-xcb-dev libxrandr-dev \
  libxshmfence-dev libxxf86vm-dev libwayland-dev wayland-protocols libwayland-egl-backend-dev glslang-tools \n  libegl-dev libgles-dev libegl-mesa0 libgl1-mesa-dri
```

Order: prerequisites, then `build_tools.sh`, `build_toolchains.sh` and `build_mesa.sh` (independent, can run together; the toolchains take the longest, a few hours), then `patch_drmshim.sh` and `run_shimtest.sh`; `build_vbcc.sh` after the toolchains. The last four packages are for the corpus's reference renders (`tests/corpus`, Mesa llvmpipe through EGL).

Installed and verified on 2026-10-09/10 (`~/toolchain-build/summary.log`):

| Toolchain | amiga-gcc | gcc | C runtime | Use |
|---|---|---|---|---|
| `~/amiga-gcc-install` | `6a2b3fb` (2026-10-04) | GCC 6.5.0b, `amiga6` `eb241170d` | libnix; clib2 `c026659` + `clib2-ndk32.patch` | ogles2 runtime, PiStorm3D |
| `~/amiga-gcc162-install` | `6a2b3fb` (2026-10-04) | GCC 16.2.0b, `amiga16.2` `134541b3686` | libnix, libstdc++ (clib2 does not build) | compiler module (C++17) |
| `~/amiga-gcc16-install` | `6a2b3fb` (2026-10-04) | GCC 16.1.1b, `amiga16.1` `a2807124780` | libnix, libstdc++ | **do not use**: miscompiles |

**GCC 16.1 miscompiles**: functions write callee-saved registers they never save (details and a ten-line reproducer in `dev/spike-c/README.md`). GCC 16.2 and GCC 6.5.0b pass the same check. Both bebbo branches keep moving, so re-run `dev/spike-c/scan_asm.sh` after updating a toolchain.

Check of the GCC 6 toolchain: the PiStorm3D 29.1 driver (`pistorm3d-driver` `01f5a3e`) built with its own Makefile appears byte-for-byte in the shipped 29.1 `minigl.library` from Aminet (27 of 29 objects, all relocation-free code). In `others.o` (93%) only `GLGetIntegerv` differs, most likely a source change between the library build (2026-10-05) and the commit (2026-10-08). `aclip.o` is not in the library because nothing references its one function, `AE_ClipTriangle`.
