# Stream C spike: Mesa's shader compiler on m68k-amigaos

Plan section 8.4. Question: can Mesa's GLSL front end, NIR and the V3D back end be built for
AmigaOS 68k with GCC 16, how large are they, and what is missing?

Everything runs in WSL. The spike replays the compile commands Meson recorded for the host
Mesa build (`~/mesa-build/mesa/build/compile_commands.json`, commit `74b6971`), rewritten
for the m68k cross-compiler. Generated sources (NIR opcode tables, GLSL lexer and parser)
come from that host build, so no Python or flex/bison runs for the target.

**Toolchain: GCC 16.2.0b** (bebbo `amiga16.2` at `134541b3686`, `~/amiga-gcc162-install`).
GCC 16.1 (`amiga16.1`) miscompiles and must not be used (see Results).

| File | Purpose |
|---|---|
| `run_all.sh` | Whole spike, parts 1 and 2: compile, link, run under vamos, compare with x86-64 (default output `~/spike-c162`) |
| `replay.py` | Rewrites and runs the compile commands (`M68K_TC` selects the toolchain, `--asm` emits assembly) |
| `classify.py` | Groups compile errors by message |
| `link.sh` | Archives the objects per library and links Mesa's standalone `glsl_compiler` |
| `size_and_run.sh` | Size breakdown from the link map; runs `glsl_compiler` under vamos |
| `compare_host.sh`, `shaders/` | Runs the m68k `glsl_compiler` (vamos) and the x86-64 one on the same shaders and compares the output |
| `top_objects.sh` | Largest objects, m68k next to x86-64 |
| `scan_asm.sh`, `check_saves_asm.py` | Callee-saved-register check on GCC's assembly output, for GCC 16.1, GCC 16.2 and GCC 6.5.0b |
| `check_saves.py`, `scan_all.sh`, `rescan_asm.sh` | The same check on disassembly (first version; disassembly misreads jump tables and string data, use the assembly check) |
| `compat/` | Compatibility layer: `amiga_compat.h` (force-included), `amiga_compat.c`, `amiga_malloc.c`, `qsort.c`, `os_amiga.c`, `fenv.h`, `xf86drm.h` |
| `patches/` | Mesa source patches, applied in `~/mesa-build/mesa`; 0001-0004 and 0006-0008 are portability fixes with no effect on x86 (0006 and 0008 are bug fixes worth sending upstream), 0005 is for the standalone test tool only, 0009 is the `.v3dp` writer's variant hook and 0010 a 68k struct-size fix (part 3) |

## Results (2026-10-10)

**Compiles.** All 450 source files of `util`, `c11`, `blake3`, `compiler`, `nir` (237),
`glcpp`, `glsl` (71), the V3D compiler (28), `qpu`, `common` and the standalone tool build for
`-mcpu=68020 -msoft-float -mcrt=nix20 -fno-exceptions -fno-rtti` (software floating point, see
part 2 and the A1200 runs; part 1 first used `-m68881`). Seven files are left out on
purpose (`replay.py`, `EXCLUDE`): ASTC decoding, DRM sync and submission, Mesa's disk-cache
database, and `os_time.c`/`os_misc.c`, which `compat/os_amiga.c` replaces.

What it took:

| Problem | Fix |
|---|---|
| `u_endian.h` knows no AmigaOS | `-DUTIL_ARCH_BIG_ENDIAN=1 -DUTIL_ARCH_LITTLE_ENDIAN=0` |
| libnix defines `struct timespec` | `-DHAVE_STRUCT_TIMESPEC` |
| `uint32_t` is `unsigned long` in newlib for m68k; Mesa assumes `unsigned int` (C++ errors, GCC 14+ pointer-type errors) | Redefine `__INT32_TYPE__`/`__UINT32_TYPE__` as `int`/`unsigned int` (same size and alignment) |
| m68k aligns 32-bit fields to 2 bytes; two Mesa size checks assume 4 | `patches/0001` (`aligned(4)` on a Vulkan-only struct), `patches/0004` (skip one size guard on m68k) |
| `CPU_TO_LE32` needs `<sys/endian.h>` on unknown platforms | `patches/0002` (`__builtin_bswap32` on AmigaOS) |
| `unsigned *` vs `uint32_t *` in `ir_constant_expression.cpp` | `patches/0003` |
| libnix headers omit prototypes of `strndup`, `vasprintf`, `nanosleep`; no `alloca` macro | `compat/amiga_compat.h` |
| No `<fenv.h>` | `compat/fenv.h` (68881 traps are off by default; holding exceptions is a no-op) |
| `v3d_util.h` includes `<xf86drm.h>` for one unused inline | `compat/xf86drm.h` |
| The hunk object format cannot align static objects beyond 8 bytes (GCC 16.2 rejects it, GCC 16.1 silently used 8); the standalone tool has a static `struct gl_context` | `patches/0005` (heap allocation; test tool only) |

**Links.** Mesa's standalone `glsl_compiler` (front end, linker, NIR, util) links with zero
undefined symbols after `compat/` supplies 11 functions: `os_get_option`,
`os_get_option_cached`, `os_log_message` (getenv reads `ENV:`), `log2f`, `exp2f` (libnix's libm
lacks them; `exp2f` via `exp` may differ from glibc in the last bit, which matters only when NIR
folds `exp2` of a constant), three GL shader-include stubs, `mkstemps`, `__xpg_strerror_r`, and
`__sync_val_compare_and_swap_1`. bebbo's GCC 16 driver links its own `new_op.o`, which
duplicates two libstdc++ symbols; the link uses `--allow-multiple-definition`.

**Size.** 11.2 MB stripped, 8.5 MB of it NIR. One generated file dominates:
`nir_opt_algebraic.c` is 7.2 MB on m68k and 7.3 MB on x86-64 (Mesa's algebraic-rule tables for
all drivers, mostly read-only data). m68k objects are the same size as x86-64 or smaller. On the
test machine (about 1.9 GB of fast RAM) this is a load-time question, about a second once, not a
memory question. Trimming the rule tables to what V3D uses is the lever if it matters.

**Runs and matches x86-64.** Under vamos (`-C 68040`), the m68k `glsl_compiler` compiles,
dumps HIR and LIR, and links the four test programs in `shaders/` (texturing with `discard`,
per-vertex lighting with a loop over uniform arrays, skinning with dynamically indexed matrix
arrays, procedural maths with constant `exp2`/`log2`). **All 17 outputs are identical to the
x86-64 build's** (`compare_host.sh`; AmigaOS has no separate stderr, so the m68k output is
compared with the host's stderr followed by its stdout).

**Problems found on the way, all fixed:**

1. **GCC 16.1 (`amiga16.1`, `a2807124780`) miscompiles.** Functions write callee-saved
   registers (d2-d7, a2-a6) that their prologue never saves. A ten-line test reproduces it
   (`leaf` saves d2 but uses d3); in Mesa, `new_sig` in `builtin_functions.cpp` clobbers a2/a3
   and its caller then jumps through the trashed a3. No `-f`/`-O` option avoids it. The
   assembly check finds 1,688 such functions in the Mesa sources with GCC 16.1 and **none with
   GCC 16.2** (`amiga16.2` has a September rework of exactly this: which registers the prologue
   saves). GCC 6.5.0b, the runtime toolchain, has none in the 29 PiStorm3D driver files either.
   GCC 16.1 also crashed (internal compiler error) on `nir_validate.c`; GCC 16.2 does not.
2. **ralloc needs 8-byte-aligned `malloc`.** Mesa's ralloc header is `alignas(8)` and its linear
   allocator asserts 8-byte alignment; libnix's `malloc` does not guarantee it.
   `compat/amiga_malloc.c` aligns explicitly. The real module takes memory from
   `ogles2.library` (plan section 8.4), and that allocator must align to 8.
3. **A Mesa comparator bug that libnix's `qsort` exposes.** `compare_intr` in
   `nir_opt_vectorize_io.c` returned -1 for an element compared with itself; glibc's sort never
   does that comparison, libnix's quicksort does, and its partitioning then runs past the array.
   `patches/0006` fixes the comparator (worth sending upstream), and `compat/qsort.c` replaces
   libnix's `qsort` with a stable merge sort that stays in bounds whatever the comparator does.
4. **Exit order.** C++ destructors and libnix's own exit code free memory after `atexit`
   handlers run; the spike allocator ignores frees once its pool is gone.

## Part 2 (2026-10-10): the whole V3D compile path, GLSL to QPU code

Instead of writing GL linker glue, part 2 runs **Mesa's own GL core, state tracker and gallium V3D
driver** on 68k, over a fake DRM device. That is the code path the offline compiler (S1) uses on
Linux, so the two share every line of compiler and driver code.

| File | Purpose |
|---|---|
| `compat/fake_drm.c` | A fake V3D 4.2 DRM device: `drmIoctl`, `mmap`, sync objects. Modelled on Mesa's drm-shim (`v3d_noop.c`); jobs "complete" without running |
| `compat/driver_stubs.c` | Debug- and display-only stubs (command-list decoder, renderonly, ASTC tables), regex, `pthread_sigmask`, 4- and 8-byte compare-and-swap |
| `compat/sys/ioccom.h`, `sys/mman.h`, `err.h`, `libsync.h`, `xf86drm*.h` | Headers libnix lacks |
| `harness/v3dc.c` | The test program (m68k, and natively on the PC since part 3): V3D screen on the fake device, surfaceless GLES2 context through the state tracker, Mesa's GL entry points called directly; `-o <dir>` writes `.v3dp` files |
| `harness/s1ref.c` | x86-64 reference: EGL surfaceless + libGLESv2 under drm-shim |
| `harness/v3dc_common.c` | The GLES2 calls both run (compile, link, logs) |
| `harness/build_v3dc.sh`, `compare_v3d.sh` | Build `v3dc`; run both with `V3D_DEBUG=precompile,qpu,shaderdb` and compare |
| `harness/check_a1200.sh` | Compares an A1200 run of `a1200/out/ogles2-spike-2.lha` with x86-64 |

**Result: identical.** On the four test programs (12 shaders: vertex, binning and fragment), the
68k build prints exactly what the x86-64 build prints: 874 QPU instructions, their uniform
streams, and the shader-db statistics (instruction, thread, uniform and register counts). 911
source files; `v3dc` is 18.8 MB stripped and needs no undefined symbol.

**Found and fixed on the way:**

1. **A big-endian bug in Mesa's V3D compiler** (`patches/0008`). The TMU configuration words are
   packed with the CLE packers, which write GPU (little-endian) bytes, into a `uint32_t` that the
   code then uses as a number (`p0_packed |= texture_idx << 24`). On 68k the texture index and the
   config bits swapped places, which would break texturing. Each packed word is now converted to
   host order (`util_le32_to_cpu`, no-op on x86). The same class of problem as plan section 7.4.
2. **Float constant folding must round every operation to single precision**, as x86-64 SSE
   does. With `-mcpu=68020 -m68881` GCC evaluates float expressions in 80-bit precision
   (`FLT_EVAL_METHOD 2`), so `mix(0.1, 0.9, 0.25)` folded to `0x3e999999` instead of
   `0x3e99999a`. 68040 code (`fsadd`/`fsmul`) fixed it in vamos, but on the A1200 the same
   difference came back: **Emu68 computes the 68040's single-precision instructions in double**
   (its FADD/FMUL handlers decode the precision and discard it, `(void)precision`, in
   `src/M68k_LINEF.c`). The build now uses software floating point (`-msoft-float`): libgcc's
   routines are exact IEEE and independent of any FPU emulation, and the compiler is mostly
   integer code. Identical to x86-64 on all four programs in vamos; the A1200 confirms it.
3. **vamos mis-emulates `fssqrt`** (it returns its input). The build uses
   `-fno-builtin-sqrt -fno-builtin-sqrtf`, so square roots go through libm (extended precision,
   then one rounding, which is exact for square roots). Emu68 is not affected; the A1200 run
   checks it.
4. **Mesa's on-disk shader cache** must be off for reference runs (`MESA_SHADER_CACHE_DISABLE`):
   with it, a second run loads shaders instead of compiling them and prints nothing. The native
   `v3dc` turns it off itself, and patch 0009 bypasses the caches while its hook is set.
5. **The hunk format's 8-byte alignment limit** also hit `tgsi_exec` (`patches/0007`), and the
   spike allocator registered an exit handler during exit (fixed in `compat/amiga_malloc.c`).

## Part 3 (2026-10-10): `.v3dp` files, the start of stream B

`v3dc -o <dir>` writes each program as `<dir>/<vs>-<fs>.v3dp` (format: `dev/v3dp/FORMAT.md`,
writer: `dev/v3dp/v3dp_writer.c`). After linking it draws triangles, lines and points into an
RGBA8 + depth framebuffer object on the fake device, with every sampler on its own unit and a
complete texture bound, so the driver compiles the variants real draws need. The variants come
from `patches/0009` (a hook called after every compile in the V3D driver, caches bypassed while
set; identical variants are dropped); the program tables are read from Mesa's linked program.

The draws found one more 68k problem: gallium's state-object cache hashes state structs as
32-bit words and asserted their size is a multiple of 4, which 2-byte alignment breaks
(`patches/0010`, which also folds in the trailing bytes; no effect on x86). On AmigaOS `v3dc`
now line-buffers `stdout`, so an assertion's message reaches the log before `abort()`.
`shaders/state.*` exercises the remaining tables: state variables, a cube sampler, a matrix
attribute, struct, bool and int uniforms, `gl_PointSize`.

| File | Purpose |
|---|---|
| `patches/0009-v3d-compiled-variant-hook.patch` | `v3d_compiled_variant_hook` in `v3d_program.c` |
| `patches/0010-cso-key-hash-m68k-struct-size.patch` | `cso_construct_key` accepts struct sizes that are not a multiple of 4 |
| `harness/build_v3dc_host.sh` | Builds `v3dc` natively (`~/spike-c162/host/v3dc`) from the host Mesa build's objects, with `compat/fake_drm.c` (its `mmap`/`munmap` pass other callers to the kernel on Linux) |
| `harness/compare_v3dp.sh` | Writes the four test programs with both builds (68k under vamos), compares the files byte for byte, decodes them with `dev/v3dp/v3dp_dump.py` |
| `harness/check_a1200_v3dp.sh` | Compares the A1200's files (`a1200/out/ogles2-v3dp-1.lha`) with the x86-64 ones |

**Result: identical**, byte for byte, for all five programs (4.6 to 11.6 KB each, five or seven
variants per program), and the independent reader decodes every file. The first, link-time-only
version was also identical on the A1200 (`a1200/results/ogles2-v3dp-1`). The test scripts give the shaders
LF line ends, because the program hash covers the exact source text.

Rebuild after changing Mesa or the writer: `replay.py --libs v3d_driver,gallium_aux,mesa` (for
patches 0009 and 0010), then
`harness/build_v3dc.sh`, `harness/build_v3dc_host.sh`, `harness/compare_v3dp.sh`.

## On the A1200 (2026-10-10, results in `a1200/results/`)

- **spike-1 (front end):** all 11 outputs identical to x86-64 (`check_a1200_spike1.sh`). About
  0.58 s per run, almost all of it loading the 11 MB program and building the built-in functions.
- **spike-2 (whole V3D path):** start-up 1.06 s; one program including start-up 1.21 to 1.35 s;
  all four programs in one run 1.77 s, so about 0.2 s of compile time per program.
- The first spike-2 run captured no GPU code: on AmigaOS libnix sends `stderr` to the console,
  not into the shell's `>file` redirection, and all output went to `stderr`. `v3dc` now points
  `stderr` at `stdout` at start-up; the package is rebuilt.
- Free fast memory fell by about 0.36 MB per spike-2 run. `memleak.py` on a traced vamos run shows
  the program frees every exec allocation; the rebuilt package logs memory per run to tell a
  filesystem cache from a leak.

- **Soft-float build on the A1200:** the GPU code of all four programs (874 QPU instructions) is
  **identical to x86-64** (`harness/check_a1200.sh`). Software floating point costs about 3%
  (all four programs 1.69 s against 1.64 s with the FPU). Memory: a one-time drop after the
  first run, then flat over 15 runs.

## Not covered yet

- The final module shape and size (plan section 8.4); this is M3 work, the feasibility questions
  are answered.
- **The final module shape**: a library with its own memory call table instead of libnix's
  program startup (plan section 8.4), and a smaller build (drop what GLES2 never uses).
