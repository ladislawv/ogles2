#!/bin/bash
# Callee-saved-register check on assembly output, for both compilers in use.
# Re-run after any toolchain update: GCC 16.1 (amiga16.1) failed it, which is
# why it is not used (dev/host-setup/README.md).
#   usage: scan_asm.sh [GCC 6 source root, default ~/check/src: PiStorm3D 29.1 driver]
D="$(dirname "$(readlink -f "$0")")"
C="$D/check_saves_asm.py"
B=${OUT_ROOT:-$HOME/ogles2-build}

# GCC 16.2: the Mesa sources of the compiler module and v3dglslc.
out=$B/asm-gcc16.2
rm -rf "$out" && mkdir -p "$out"
M68K_TC=$HOME/amiga-gcc162-install python3 -I "$D/replay.py" --asm --out "$out" | tail -1
find "$out/obj" -name '*.o' -print0 | xargs -0 python3 -I "$C" > "$out/report.txt"
echo "== gcc16.2 Mesa sources: $(tail -1 "$out/report.txt")"
grep -v "^suspect" "$out/report.txt" | head -4

# GCC 6.5.0b: C sources with the PiStorm3D Makefile's flags, as assembly.
G6=$HOME/amiga-gcc-install/bin/m68k-amigaos-gcc
SRC=${1:-$HOME/check/src}
out=$B/asm-gcc6 && rm -rf "$out" && mkdir -p "$out"
for f in $(find "$SRC" -name '*.c'); do
  $G6 -std=c99 -O2 -fno-strict-aliasing -fno-builtin-cos -fno-builtin-sin -finline-functions \
      -DMGLV3D_NO_LOGGING -mcpu=68020 -m68881 -mcrt=clib2 \
      $(find "$SRC" -type d -name include -printf '-I%p ') \
      -S "$f" -o "$out/$(basename "${f%.c}").s" 2>/dev/null
done
python3 -I "$C" "$out"/*.s > "$out/report.txt"
echo "== gcc6.5.0b $SRC: $(ls "$out"/*.s | wc -l) files, $(tail -1 "$out/report.txt")"
grep -v "^suspect" "$out/report.txt" | head -4
