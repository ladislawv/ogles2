#!/bin/bash
# Spike part 2: run the V3D driver's shader compile on m68k (v3dc under vamos,
# fake DRM device) and on x86-64 (s1ref: EGL + drm-shim, the offline
# compiler's path), on the same shader pairs, and compare the QPU code.
#   usage: compare_v3d.sh [debug flags]   (default precompile,qpu)
set -u
S=${S:-$HOME/spike-c162}
FLAGS=${1:-precompile,qpu}
H="$(dirname "$(readlink -f "$0")")"
SH="$(dirname "$H")/shaders"
MI=~/mesa-install
L=$MI/lib/x86_64-linux-gnu
W=$S/v3dcmp && rm -rf $W && mkdir -p $W && cd $W
cp "$SH"/*.vert "$SH"/*.frag . || exit 1
PAIRS="tex.vert tex.frag light.vert light.frag skin.vert skin.frag tex.vert proc.frag"

# ---- x86-64 reference. The on-disk shader cache must be off, or a second run
# loads the shaders instead of compiling them and prints no QPU code.
gcc -O1 -Wall -I"$MI/include" -I"$H" "$H/s1ref.c" -L"$L" -Wl,-rpath-link,"$L" -Wl,-rpath,"$L" \
    -lEGL -lGLESv2 -o s1ref || exit 1
LD_LIBRARY_PATH="$L" LD_PRELOAD="$L/libv3d_noop_drm_shim.so" V3D_GPU_ID=42 \
    MESA_LOADER_DRIVER_OVERRIDE=v3d MESA_SHADER_CACHE_DISABLE=true V3D_DEBUG=$FLAGS \
    ./s1ref $PAIRS > host.out 2> host.txt
echo "host s1ref rc=$? ($(wc -l < host.txt) lines)"

# ---- m68k
cp $S/v3dc.stripped v3dc
start=$(date +%s)
timeout 3000 ~/amitools-venv/bin/vamos -C 68040 -H disable -m 262144 -s 4096 -- \
    v3dc -d $FLAGS $PAIRS > m68k.txt 2> m68k.err
echo "m68k v3dc rc=$? ($(wc -l < m68k.txt) lines, $(( $(date +%s) - start )) s under vamos)"
grep -vE "WARNING|^\s*$" m68k.err | grep -E "ERROR|rror" | head -5

# ---- compare. The disassembler prints the float view of some constants;
# glibc spells NaN "-nan", libnix "NaN": normalise that spelling only.
norm() { grep -v "^v3dc: renderer" "$1" | sed -E 's#/ -?(nan|NaN)\)#/ NaN)#g'; }
norm host.txt > host.cmp
norm m68k.txt > m68k.cmp
grep "^v3dc: renderer" host.txt m68k.txt
if cmp -s host.cmp m68k.cmp; then
  echo "IDENTICAL: $(wc -l < host.cmp) lines of driver output (QPU code for $(grep -c '^v3dc: ====' host.cmp) programs)"
else
  echo "DIFFERENT:"; diff host.cmp m68k.cmp | head -40
fi
