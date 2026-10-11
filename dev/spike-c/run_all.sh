#!/bin/bash
# Whole spike with one toolchain: compile everything (part 1 compiler code and part 2
# driver path), link and run both test programs, and compare them with x86-64.
# usage: run_all.sh <toolchain root> <output dir>
#   e.g. run_all.sh ~/amiga-gcc162-install ~/spike-c162
TC=${1:-$HOME/amiga-gcc162-install}
S=${2:-$HOME/spike-c162}
D="$(dirname "$(readlink -f "$0")")"
$TC/bin/m68k-amigaos-gcc --version | head -1
rm -rf "$S" && mkdir -p "$S"
echo "== compile (part 1 + part 2)"
M68K_TC=$TC python3 -I "$D/replay.py" --out "$S" --libs all || exit 1
echo "== part 1: link glsl_compiler"
M68K_TC=$TC S=$S bash "$D/link.sh"
echo "== part 1: size and vamos run"
M68K_TC=$TC S=$S bash "$D/size_and_run.sh"
echo "== part 1: compare with x86-64"
S=$S bash "$D/compare_host.sh" | tail -1
echo "== part 2: build v3dc (GL + V3D driver on the fake DRM device)"
M68K_TC=$TC S=$S bash "$D/harness/build_v3dc.sh"
echo "== part 2: compare V3D QPU code with x86-64"
S=$S bash "$D/harness/compare_v3d.sh" precompile,qpu,shaderdb | tail -2
echo "(register-save check of all three compilers: scan_asm.sh)"
