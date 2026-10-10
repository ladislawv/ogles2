# ogles2.library for AmigaOS 3.x on PiStorm (Raspberry Pi 4, V3D 4.2)

OpenGL ES 2.0 for 68k AmigaOS, running on the Pi 4's GPU through PiStorm/Emu68, source-compatible
with AmigaOS 4's `ogles2.library`. The development plan is
`C:\- CPP\pistorm3d\OGLES2_DEVELOPMENT_PLAN.md` (outside this repository).

Status: milestone M0 (foundations), in progress.

## Layout

| Path | What |
|---|---|
| `ogles2/` | The library: resident tag and vectors (`library.c`), file log (`log.c`), contexts (`context.c`) |
| `client/` | `libogles2.a`, the import library: `OGLES2Open/Close`, current context, `agl*` |
| `include/` | Public headers: `libraries/ogles2.h`, `clib/ogles2_protos.h`; Khronos `GLES2/` and `KHR/` verbatim (registry `6af574a`, `db3425b`) |
| `gen/` | `gen_ogles2.py`: dispatch table, client wrappers, `aglGetProcAddress` thunks, stubs and ABI test, from `khronos/gl.xml`, `api.txt`, `agl.txt` and the append-only `abi_order.txt` |
| `cmd/` | `OGLES2Log`: writes the library's buffered log now |
| `v3dp/` | The `.v3dp` program-binary format (`FORMAT.md`, frozen at 1.0), writer and independent reader |
| `tools/v3dglslc/` | The GLSL compiler: Mesa's GL + V3D driver on a fake DRM device, built natively (offline compiler, S1) and for 68k (seed of the on-target compiler, S2); Mesa patches; tests |
| `tests/` | `smoke/` (Amiga smoke test), `a1200/` (test packages for the A1200) |
| `dev/host-setup/` | Build-machine setup: toolchains, Mesa, Emu68 |
| `licences/` | Licence texts and third-party notices |

## Building (WSL Ubuntu 24.04, toolchains from `dev/host-setup/`)

```sh
make -j8                                   # -> ~/ogles2-build/m68k: ogles2.library, libogles2.a, tests, OGLES2Log
make vbcc                                  # vbcc clients: ogles2.lib, smoke and abi_probe built with vbcc (dev/host-setup/build_vbcc.sh)
bash tests/a1200/build_package.sh m0-1     # -> tests/a1200/out/ogles2-m0-1.lha

# Shader compiler (needs the host Mesa build with tools/v3dglslc/patches applied)
python3 tools/v3dglslc/replay.py           # Mesa for 68k (GCC 16.2), ~900 files
bash tools/v3dglslc/build_m68k.sh          # -> ~/ogles2-build/v3dglslc/v3dglslc
bash tools/v3dglslc/build_host.sh          # -> ~/ogles2-build/v3dglslc/host/v3dglslc
bash tools/v3dglslc/test/compare_v3dp.sh   # x86-64 and 68k .v3dp files must be identical
```

The library uses GCC 6.5.0b (the compiler PiStorm3D 29.1 is built with); the compiler code uses
GCC 16.2 with software floating point. GCC 16.1 miscompiles and must not be used.

The smoke test and the ABI probe also run on the PC under vamos, with the real library loaded
(`NOGPU`: vamos has no V3D, so the context is state-only):
`vamos -C 68040 -V work:<dir> -a libs:work:libs -a T:work:t -O ogles2.library=mode:amiga -- abi_probe NOGPU`.

Clients can be built with GCC (`libogles2.a`) or vbcc (`ogles2.lib`, compile with `-c99`); the
argument passing of both is verified against the GCC-built library (`tests/abi_probe`).

## Licence

`ogles2.library` incorporates PiStorm3D's V3D backend (`v3dcore/`), which is under the Hyperion
MiniGL Open Source License (`licences/License_MiniGL_Library.txt`): the library is for AmigaOS,
and the source of all modifications is published to the Amiga developer community. Third-party
code keeps its own licence and notices (`licences/License_V3D_ThirdParty.txt`; Khronos headers
MIT, `gl.xml` Apache-2.0; Mesa MIT). The parts that incorporate no MiniGL code (`v3dp/`,
`tools/v3dglslc/`, `gen/`) are MIT.
