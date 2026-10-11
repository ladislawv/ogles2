#!/bin/bash
# Callee-saved-register check on assembly output for every compiler in use.
D="$(dirname "$(readlink -f "$0")")"
C="$D/check_saves_asm.py"
for spec in "gcc16.1:$HOME/amiga-gcc16-install" "gcc16.2:$HOME/amiga-gcc162-install"; do
  name=${spec%%:*}; tc=${spec#*:}
  out=$HOME/spike-asm-$name
  rm -rf "$out" && mkdir -p "$out"
  M68K_TC=$tc python3 -I "$D/replay.py" --asm --out "$out" | tail -1
  find "$out/obj" -name '*.o' -print0 | xargs -0 python3 -I "$C" > "$out/report.txt"
  echo "== $name Mesa compiler sources: $(tail -1 "$out/report.txt")"
  grep -v "^suspect" "$out/report.txt" | head -4
done
# GCC 6.5.0b: the PiStorm3D 29.1 driver, same flags as its Makefile, as assembly.
G6=$HOME/amiga-gcc-install/bin/m68k-amigaos-gcc
SRC=$HOME/check/src
out=$HOME/spike-asm-gcc6 && rm -rf "$out" && mkdir -p "$out"
for f in $SRC/gl/src/*.c $SRC/backend/hw/*.c; do
  $G6 -std=c99 -O2 -fno-strict-aliasing -fno-builtin-cos -fno-builtin-sin -finline-functions \
      -DMGLV3D_NO_LOGGING -mcpu=68020 -m68881 -mcrt=clib2 -I$SRC/gl/include -I$SRC/backend/include \
      -S "$f" -o "$out/$(basename "${f%.c}").s" 2>/dev/null
done
python3 -I "$C" "$out"/*.s > "$out/report.txt"
echo "== gcc6.5.0b PiStorm3D driver: $(ls "$out"/*.s | wc -l) files, $(tail -1 "$out/report.txt")"
grep -v "^suspect" "$out/report.txt" | head -4
