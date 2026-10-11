#!/bin/bash
# Compares the GPU code from an A1200 run of ogles2-spike-2 with the x86-64
# reference (s1ref under drm-shim) for the same shader pairs.
#   usage: check_a1200.sh <results dir from the A1200>
set -u
R=$(readlink -f "${1:?results dir}")
H="$(dirname "$(readlink -f "$0")")"
SH="$(dirname "$H")/shaders"
MI=~/mesa-install
L=$MI/lib/x86_64-linux-gnu
W=$(mktemp -d) && cd $W
cp "$SH"/*.vert "$SH"/*.frag .
gcc -O1 -I"$MI/include" -I"$H" "$H/s1ref.c" -L"$L" -Wl,-rpath-link,"$L" -Wl,-rpath,"$L" \
    -lEGL -lGLESv2 -o s1ref || exit 1
norm() { tr -d '\r' < "$1" | grep -v "^v3dc: renderer" | sed -E 's#/ -?(nan|NaN)\)#/ NaN)#g'; }
for spec in "tex:tex.vert tex.frag" "light:light.vert light.frag" "skin:skin.vert skin.frag" "proc:tex.vert proc.frag"; do
  name=${spec%%:*}; pair=${spec#*:}
  f="$R/${name}_qpu.txt"
  [ -f "$f" ] || { echo "$name: no result file"; continue; }
  LD_LIBRARY_PATH="$L" LD_PRELOAD="$L/libv3d_noop_drm_shim.so" V3D_GPU_ID=42 \
    MESA_LOADER_DRIVER_OVERRIDE=v3d MESA_SHADER_CACHE_DISABLE=true V3D_DEBUG=precompile,qpu,shaderdb \
    ./s1ref $pair > /dev/null 2> host_$name.txt
  norm host_$name.txt > h.cmp
  norm "$f" > a.cmp
  if cmp -s h.cmp a.cmp; then
    echo "$name: IDENTICAL ($(grep -c '^MESA: info: 0x' h.cmp) QPU instructions)"
  else
    echo "$name: DIFFERENT"; diff h.cmp a.cmp | head -12
  fi
done
echo "== times"; tr -d '\r' < "$R/times.txt" 2>/dev/null
rm -rf $W
