# Working on ogles2

This is the handbook for picking the project up: where things stand, how to set up a build
machine, how to build and test, the conventions, and the traps that already cost time.

## Where things stand

The [development plan](docs/DEVELOPMENT_PLAN.md) is the project's single design and status
document. Read §1 (goals), §7 (runtime design), §8 (the shader pipeline), §9 (milestones) and
**§13 (status and next steps)** first. §13 ends with the ordered "Next" list; take work from
there. When something is done, decided or found, update the plan (the relevant section and
§13) in the same change, rather than writing a separate document.

M0 to M3 are complete: the library runs real GLES2 programs and compiles their shaders on
the Amiga (`v3dglslc` as a resident compiler server the library starts on demand:
`ogles2/compiler.c`, `v3dp/v3dglsl_port.h`). M4 (GLES2 core complete) is next; §13 lists it.

## Setting up a build machine

Ubuntu 24.04, natively or in WSL on Windows. The repository can live anywhere (also on a
Windows drive, `/mnt/c/...`); the build output goes to `~/ogles2-build`.

1. The packages listed in [`dev/host-setup/README.md`](dev/host-setup/README.md).
2. `bash dev/host-setup/build_tools.sh`: lha and vamos (minutes).
3. `bash dev/host-setup/build_toolchains.sh`: GCC 6.5.0b with clib2 and GCC 16.2, pinned to
   the tested commits (`dev/host-setup/toolchain-pins.txt`); a few hours.
4. `bash dev/host-setup/build_mesa.sh`, then the v3dglslc steps of the README's Building section,
   for the shader compiler (needed for anything that compiles GLSL, including the corpus and
   `make test-v3dp`).
5. `bash dev/host-setup/build_vbcc.sh` if you work on the vbcc client side.
6. Optional: `bash dev/fetch_external.sh` puts the plan's reference sources (PiStorm3D,
   MiniGL glue, Mesa's V3D code, OS4 SDL and gl4es glue, glSnoop, Emu68) into `external/`.

## Building and host checks

```sh
make -j8              # library, import library, tests, commands
make amictl-test      # the test station's server and client, on the PC
make blake3-test      # ogles2's BLAKE3 against Mesa's (needs the Mesa sources)
make etc1-test        # ogles2's ETC1 decoder against Mesa's (the same)
make test-v3dp        # compiles the test shaders to .v3dp (needs v3dglslc)
make perf-mgl         # the MiniGL side of the speed comparison (needs MGL_SDK)
```

Every `make` also compiles the library to assembly and runs `tools/check_fpconv.py`, which
fails the build on a float-to-integer conversion GCC 6 has broken (see Traps).
`.github/workflows/host-checks.yml` runs the checks that need no Amiga toolchain on every push.

## Testing on the Amiga

There is no emulator for the GPU: everything that draws is tested on real hardware. Each test
is a **package**, `tests/a1200/<name>/`, with:

- `build.sh`, which fills the package directory (programs, libraries, data);
- `textfiles`, the scripts and texts copied with Amiga line ends (checked to be ASCII);
- `Run-*`, the AmigaDOS script: it adds the package's `libs/` to `LIBS:`, runs
  `Avail FLUSH`, sets `ENV:OGLES2/LogFile`, runs numbered steps (resumable with `FROM=`),
  and writes everything to `results/`;
- `ReadMe.txt` for whoever runs it, and `notes-template.txt` for their observations.

`bash tests/a1200/build_package.sh <name>` builds `tests/a1200/out/ogles2-<name>/` and its
`.lha`. Copy that to the Amiga and run it, or use the remote test station.

**Write tests a person can watch.** A visible test holds each state about a second, and the
Shell says beforehand what should appear (the step's `Echo`). Hidden tests draw into bitmaps
and check pixels. Every test ends by checking that the context returned its memory (the log
line `device: context freed, 0 bytes of GPU memory still in use`).

**Results** go to `tests/a1200/results/ogles2-<name>-<date>-<time>/` and are committed: the
plan cites them as evidence. Screenshots of the test machine's screen (`shots/`, `.png`,
`.jpg`) stay local: they show the owner's desktop (`.gitignore`).

### The remote test station

`tools/amictl` lets the PC upload a package, run it, fetch its results and take screenshots,
so nobody has to copy files by hand.

1. Make `tools/amictl/station.local` from `station.local.example`: the Amiga's address, a
   token (`python3 tools/amictl/amictl.py gentoken /dev/stdout`) and the PC's address for the
   allow list. The file is in `.gitignore`: never commit it.
2. `bash tests/a1200/build_package.sh amictld-1` and install the package on the Amiga as its
   `ReadMe.txt` says (Roadshow needed; one line in `S:User-Startup`).
3. `python3 tools/amictl/amictl.py hello`, then for example
   `python3 tools/amictl/amictl.py runpkg tests/a1200/out/ogles2-m2-4 -t 400`.

The server accepts only the allowed addresses and the token, logs every command, and must
stay on the LAN (do not forward port 6800). Ask the machine's owner before rebooting it or
changing its system files.

### The speed comparison

`tests/perf` draws the same gears scene with ogles2 (`gears_gles2`) and with PiStorm3D 29.1's
MiniGL (`gears_mgl`); package `m4-2` runs both and prints frames per second. The MiniGL side
needs the `SDK/` directory and `minigl.library` of the PiStorm3D 29.1 release (Aminet
`util/libs/PiStorm3D.lha`), by default in `~/minigl-29.1/SDK` and `~/minigl-29.1/bin`
(`MGL_SDK`, `MGL_DIR`). Never run the two libraries at the same time: the package runs one
after the other with `Avail FLUSH` between.

### The corpus

`tests/corpus` runs unmodified GLES2 programs (es2gears, the OpenGL ES 2.0 Programming Guide
samples) for a fixed number of frames, captures a frame, and compares it with Mesa llvmpipe
rendering the same program on the PC. `build_corpus.sh` fetches the programs, builds both
sides, renders the references and compiles the shaders; package `m2-5` runs them on the
Amiga; `compare.py` compares. `CORPUS_FRAMES=0` runs a program live until Esc (package
`gears`). New programs go into the `PROGRAMS` list of `build_corpus.sh`. `programs/` holds our
own programs in the book's style, for what neither source covers (framebuffer objects); on the
PC, binding framebuffer 0 binds the object the capture reads (`host_hooks.h`).

## Conventions

- **C:** `gnu99` for the library and tests, `-Wall -Wextra` clean. The library is
  freestanding: no C library (`ogles2/runtime.c` has the few string functions it needs), exec
  and DOS directly, every allocation freed by the context that made it.
- **Comments** say why, and cite the plan (`plan 7.5`) or the Mesa function a piece of logic
  follows; the GLES 2.0 spec section for API rules. Match the density of the file you edit.
- **New entry points:** write `ogles2_glFoo(struct OGLES2Context *ctx, ...)` in any file of
  `LIB_SRC`; the generator finds it and replaces the stub. `gen/abi_order.txt` is
  append-only: never reorder or delete (retire an entry as `-name`). An extension's entry
  point also needs a `command <name>` line in `gen/api.txt`, and one run of
  `python3 gen/gen_ogles2.py --out <dir> --impl-dir ogles2 --append` to add it to the order.
- **GPU data** is written little-endian (`le32`/`LE32`); the 68k is big-endian.
- **Logging:** `ogles2_log()` for anything a tester or developer would need after the fact,
  once per cause for things that can repeat every frame.
- **Line ends:** LF everywhere (`.gitattributes`). Amiga scripts need it, and a `.v3dp`
  file is found by the hash of the exact shader bytes.
- **Commits:** a subject that says what changed, a body that says why; reference the plan
  section and the A1200 run that verified it.

## Traps

Each of these cost real time once.

- **A stale library in memory.** AmigaOS keeps an opened library resident; with plenty of
  RAM nothing expunges it, and `OpenLibrary` returns the old one even when `LIBS:` points at a
  new file. Every `Run-*` script runs `Avail FLUSH` first, and a log without the
  `ogles2.library ... init` line means the run used an old library.
- **GCC 6 drops `fintrz`.** bebbo's optimisation pass `e` deletes the truncation of
  float-to-int conversions, so `(int)255.5f` becomes 256. Everything is built with
  `-fbbb=abcfhilmnprsz0` and `check_fpconv.py` guards it.
- **`-lm881`, not `-lm`,** for clib2 programs built with `-m68881`: `-lm` silently links
  libnix's maths, which gives wrong results under clib2 (`sqrtf(2)` = 0).
- **`-fno-builtin`** for programs that call both `sinf` and `cosf`: GCC merges them into
  `cexpf`, which clib2 lacks.
- **GCC 16.1 miscompiles** (callee-saved registers not saved; `dev/spike-c/README.md`). Use
  GCC 16.2; after any toolchain update run `tools/v3dglslc/scan_asm.sh`.
- **FFS file names are at most 30 characters:** `.v3dp` files are named by the first 24
  hex digits of the hash.
- **Never use the Flip Y raster store:** it crashed the machine (M1 hardware probe Y).
- **Rebooting the Amiga does not reset the GPU.** Emu68 restarts the 68k side; the Raspberry
  Pi and its V3D keep running. A job the GPU never finished used to leave it stopped for every
  later program, across reboots. The library now resets the GPU after such a job and when it
  finds it stopped at device open (`device: GPU reset ...` in the log); if a GPU problem
  survives that, only switching the machine off clears it.
- **vamos has no FPU:** test FPU code (maths, float conversions) on the Amiga.
- **Paths with spaces** break unquoted shell commands; `build_corpus.sh` reaches the
  repository through a link for that reason.
- **Large screenshots over amictl sometimes stall** (an open issue in §13); smaller results
  come through.

## Licences

Code that links with `v3dcore/` is under the Hyperion MiniGL Open Source License: it may only
be built for AmigaOS, and all modifications must be published. Keep third-party notices in
place (`licences/`, `v3dcore/PROVENANCE.md`), and do not commit code whose licence does not
allow redistribution (the corpus fetches such programs at build time instead). See
[`LICENSE`](LICENSE).
