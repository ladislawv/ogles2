#!/bin/bash
# Compare the m68k glsl_compiler (under vamos) with the x86-64 one from the host
# Mesa build, on the same shaders and options. Identical text = the front end and
# linker behave the same on big-endian m68k.
# usage: compare_host.sh [spike dir]
S=${1:-$HOME/spike-c162}
B=~/mesa-build/mesa/build
. ~/mesa-build/venv/bin/activate
ninja -C $B src/compiler/glsl/glsl_compiler > $S/host_build.log 2>&1 || { echo "host build failed"; tail -5 $S/host_build.log; exit 1; }
HOST=$B/src/compiler/glsl/glsl_compiler
V="$HOME/amitools-venv/bin/vamos -C 68040 -H disable -m 262144 -s 2048 --"
SH="$(dirname "$(readlink -f "$0")")/shaders"
mkdir -p $S/cmp && cd $S/cmp
cp "$SH"/*.vert "$SH"/*.frag . || exit 1
cp $S/glsl_compiler.stripped glsl_compiler
pass=0; fail=0
run_case() {  # name, args...
    local name=$1; shift
    # AmigaOS has no separate stderr: libnix writes it, unbuffered, into the same
    # stream as stdout. Compare against the host's stderr followed by its stdout.
    $HOST "$@" > host_$name.out 2> host_$name.err; local hr=$?
    cat host_$name.err host_$name.out > host_$name.txt
    $V glsl_compiler "$@" > m68k_$name.txt 2> m68k_$name.err; local mr=$?
    if [ $hr -eq $mr ] && cmp -s host_$name.txt m68k_$name.txt; then
        pass=$((pass+1)); echo "SAME  $name ($(wc -l < host_$name.txt) lines, rc=$hr)"
    else
        fail=$((fail+1)); echo "DIFF  $name (rc host=$hr m68k=$mr)"
        diff host_$name.txt m68k_$name.txt | head -8
        grep -vE "WARNING" m68k_$name.err | tail -3
    fi
}
for f in *.frag *.vert; do
    base=${f%.*}_${f##*.}
    run_case "${base}_hir" --version 100 --dump-hir "$f"
    run_case "${base}_lir" --version 100 --dump-lir "$f"
done
for v in *.vert; do
    fr=${v%.vert}.frag
    [ -f "$fr" ] && run_case "${v%.vert}_link_lir" --version 100 --link --dump-lir "$v" "$fr"
done
echo "same: $pass  different: $fail"
