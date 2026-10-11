#!/bin/bash
# Compares an A1200 run of ogles2-spike-1 (results/<case>.txt, stdout only: on
# AmigaOS libnix sends stderr to the console, not into the redirection) with
# the x86-64 glsl_compiler's stdout for the same cases.
#   usage: check_a1200_spike1.sh <results dir>
set -u
R=$(readlink -f "${1:?results dir}")
B=~/mesa-build/mesa/build
HOST=$B/src/compiler/glsl/glsl_compiler
SH="$(dirname "$(readlink -f "$0")")/shaders"
W=$(mktemp -d) && cd $W && cp "$SH"/*.vert "$SH"/*.frag .
same=0; diff=0
check() {  # name, args...
    local name=$1; shift
    [ -f "$R/$name.txt" ] || { echo "$name: no result file"; return; }
    $HOST "$@" > host.txt 2>/dev/null
    if cmp -s host.txt <(tr -d '\r' < "$R/$name.txt"); then
        same=$((same+1)); echo "SAME  $name"
    else
        diff=$((diff+1)); echo "DIFF  $name"; diff host.txt <(tr -d '\r' < "$R/$name.txt") | head -6
    fi
}
check empty_first --version 100 --dump-lir empty.frag
for f in tex.vert tex.frag light.vert light.frag skin.vert skin.frag proc.frag; do
    check "${f%.*}_${f##*.}_lir" --version 100 --dump-lir "$f"
done
for p in tex light skin; do
    check "${p}_link_lir" --version 100 --link --dump-lir $p.vert $p.frag
done
echo "same: $same  different: $diff"
rm -rf $W
