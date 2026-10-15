# Working on ogles2

This is the handbook for picking the project up: where things stand, how to set up a build
machine, how to build and test, the conventions, and the traps that already cost time.

## Where things stand

The [development plan](docs/DEVELOPMENT_PLAN.md) is the project's single design and status
document. Read §1 (goals), §7 (runtime design), §8 (the shader pipeline), §9 (milestones) and
**§13 (status and next steps)** first. §13 ends with the ordered "Next" list; take work from
there. When something is done, decided or found, update the plan (the relevant section and
§13) in the same change, rather than writing a separate document.

M0 to M6 are complete (M6's optional rest aside), and M7 nearly: the library runs real GLES2
programs and compiles their shaders on the Amiga (`v3dglslc`, a resident compiler server the
library starts on demand), with framebuffer objects, 4x MSAA, sharing across contexts and
tasks, SDL2/SDL3 backends, gl4es, glmark2, and Quake III both through gl4es and with
ioquake3's GLES2 renderer directly (`ports/ioq3-gl2`). dEQP-GLES2's must-pass list passes
except the documented deviations, and the release archive (library, compiler, installer, SDK)
is built by `release/build_release.sh`. What is left (publishing the release; the test
station's large-reply stall, postponed) is in §13's "Next" list.
[`CLAUDE.md`](CLAUDE.md) has the short version for an AI agent.

## Setting up a build machine

Ubuntu 24.04, natively or in WSL on Windows. The repository can live anywhere (also on a
Windows drive, `/mnt/c/...`); the build output goes to `~/ogles2-build`.

1. The packages listed in [`dev/host-setup/README.md`](dev/host-setup/README.md).
2. `bash dev/host-setup/build_tools.sh`: lha and vamos (minutes).
3. `bash dev/host-setup/build_toolchains.sh`: GCC 6.5.0b with clib2 and GCC 16.2, pinned to
   the tested commits (`dev/host-setup/toolchain-pins.txt`); a few hours.
4. `bash dev/host-setup/build_mesa.sh`, then
   `for p in tools/v3dglslc/patches/*.patch; do git -C ~/mesa-build/mesa apply "$p"; done`
   (the v3dglslc builds expect them in that tree), then the v3dglslc steps of the README's
   Building section, for the shader compiler (needed for anything that compiles GLSL,
   including the corpus and `make test-v3dp`).
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
make gl4es            # gl4es for AmigaOS 3 and gears_gl4es (ports/gl4es/README.md)
bash ports/ioq3/build.sh       # Quake III on gl4es (after make gl4es): ~/ogles2-build/m68k/ioq3
bash ports/ioq3-gl2/build.sh   # Quake III with renderergl2 (after ports/ioq3): .../ioq3gl2
bash release/build_release.sh  # the release archives in ~/ogles2-build/release ("dist": also release/dist/)
```

The release builder needs the shader compiler built both ways (`tools/v3dglslc/build_m68k.sh`
and `build_host.sh`, checked identical by `compare_v3dp.sh`): the m68k one ships as
`Libs/v3dglslc`, the host one compiles the examples' shaders. It builds the examples from the
SDK as assembled, with GCC and vbcc, so a broken SDK fails the build. The release's texts are
in `release/` (`ReadMe`, `Install-ogles2`, the autodoc `ogles2.doc`, the guide
`GettingStarted.txt`), its examples in `release/examples/`; keep `ogles2.doc` and the guide in
step with the library's tags, extensions and deviations. It makes three archives: `ogles2-<v>.lha`
(users), `ogles2-<v>-sdk.lha` (with `SDK/`) and `ogles2-<v>-sdk.zip` (`SDK/` alone, for
Windows). `release/dist/` holds the current ones, committed for those who work on Windows
without the build machine's output: refresh them with `build_release.sh dist` when a release
changes, not on every build (each refresh adds about 10 MB to the history).

The ports fetch their sources on first build, each at a pinned commit: gl4es and SDL2/SDL3
from GitHub (their `build.sh`), Quake III through `ports/ioq3/sources.sh` (Cowcat's AmigaOS 3
port with the PiStorm changes, github.com/ladislawv/ioq3-amiga, into `~/ioq3-amiga`;
upstream ioquake3 into `~/ioq3-upstream`). `IOQ3_SRC` and `IOQ3_UP_SRC` point them at trees
of your own instead. Changes to those sources are patches in `ports/ioq3*/patches/`.
The Quake III game data (`baseq3/pak*.pk3`, a retail copy) is only needed on the Amiga.

**Access:** `ogles2` and `ioq3-amiga` are private GitHub repositories; ask the owner to add
you as a collaborator. Git inside WSL then needs your GitHub login, which it can take from
Git for Windows' credential manager (one line, once):

```sh
git config --global credential.helper "/mnt/c/Program\ Files/Git/mingw64/bin/git-credential-manager.exe"
```

Newer Git for Windows installs it under `ucrt64/bin` instead of `mingw64/bin`.

Without it a clone of a private repository fails at once (the scripts never wait for a
password prompt): `could not clone ... needs git credentials in WSL`.

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

**The release** is tested from its archive, unpacked on the Amiga with its LhA: once with
nothing installed (the examples run with `ogles2.library`, and `v3dglslc` if they need it, in
their drawer or its `libs/`: `OGLES2Open()` and the compiler look there when `LIBS:` has
none), and once installed with `Execute Install-ogles2 ALL` (the station cannot answer the
script's `Ask`s: its commands' input is empty, which answers no to each). §13 has the 1.3
run.

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
- **Ports built with their own flags miss `-fbbb=abcfhilmnprsz0`** and get pass `e`'s rounding
  conversions (above): Quake III's renderergl2 packed colours of 1.0 as 0 (invisible
  sprites). Every port is built with the flag (`ports/ioq3*/build.sh` too).
- **PowerShell mangles `$`, `\` and quotes** in `wsl bash -c "..."`: write the commands to a
  script file and run `wsl -d Ubuntu-24.04 bash <file>` (the distribution's name as `wsl -l`
  lists it: plain `Ubuntu` on some machines).
- **Unstable memory looks like compiler bugs.** On a second build machine (2026-10-12) the
  toolchain and Mesa builds failed at random: internal compiler errors and assembler crashes in
  different files each run, a flipped bit in GCC's assembly output, a corrupt object. With the
  RAM's XMP/DOCP profile off (four DIMMs) the errors became rare but did not stop: a failure that
  does not repeat when the same step is run again is the machine, not the code. Check: compile
  one large file (Mesa's `nir_opt_algebraic.c`) 100 times, 16 at a time: every object must be
  byte-identical. A toolchain built on such a machine may miscompile quietly: `compare_v3dp.sh`
  and `scan_asm.sh` check it end to end.
- **Line ends:** Windows Python (and some editors) write CRLF into files that must be LF.
  Patches to ported sources are made against the clones the builds use (in WSL); a Windows
  checkout of the same repository has CRLF (git's `core.autocrlf`), and a patch made there
  does not apply. `ioq3-amiga` itself mixes CRLF and LF and is kept byte for byte.
- **Quake III keeps settings** in its home's `baseq3/q3config.cfg` from run to run (archived
  cvars): delete it before a comparison. A timedemo runs faster than real time, so its sound
  is garbled whatever the driver; judge sound by playing.
- **Two GL programs at once** (a game left running and a test started) can hang the Amiga:
  check with `amictl.py run Status` before starting one.
- **An installed ogles2 overrides the packages'.** `Run-*` scripts add the package's `libs/`
  with `Assign LIBS: ... ADD`, which goes after the system's `Libs`: with
  `LIBS:ogles2.library` installed (the release's installer) every package runs that one, and
  with `LIBS:v3dglslc` installed the compiler is always there. Before running packages, check
  `List LIBS:ogles2#? LIBS:v3dglslc` on the station; uninstall (ReadMe, "Installing") to test
  a build.

## Licences

New code is MIT. Code that adapts PiStorm3D's or MiniGL's code is under the Hyperion MiniGL
Open Source License (it may only be built for AmigaOS, and all modifications must be
published): `v3dcore/` and the files `LICENSE` lists in section 1. Adapting such code into
another file moves that file to section 1: say so in its opening comment and in `LICENSE`. Keep
`client/` and the public headers free of it, so programs of any licence can link them. Keep
third-party notices in place (`licences/`, `v3dcore/PROVENANCE.md`), and do not commit code
whose licence does not allow redistribution (the corpus fetches such programs at build time
instead). See [`LICENSE`](LICENSE).
