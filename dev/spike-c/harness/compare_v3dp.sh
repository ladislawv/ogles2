#!/bin/bash
# Stream B check: write .v3dp files for the test programs with the native
# x86-64 v3dc (build_v3dc_host.sh) and the m68k v3dc under vamos
# (build_v3dc.sh), and require them to be identical byte for byte. Then
# decode the host files with the independent reader (dev/v3dp/v3dp_dump.py).
#   usage: compare_v3dp.sh
set -u
S=${S:-$HOME/spike-c162}
H="$(dirname "$(readlink -f "$0")")"
SH="$(dirname "$H")/shaders"
V="$(dirname "$(dirname "$H")")/v3dp"
W=$S/v3dpcmp && rm -rf $W && mkdir -p $W/host $W/m68k && cd $W
cp "$SH"/*.vert "$SH"/*.frag . || exit 1
# The source hash covers the exact text: use LF line ends, as the A1200 packages do.
sed -i 's/\r$//' *.vert *.frag
PAIRS="tex.vert tex.frag light.vert light.frag skin.vert skin.frag tex.vert proc.frag state.vert state.frag"

$S/host/v3dc -o host $PAIRS > host.txt 2>&1
echo "host v3dc rc=$?"

cp $S/v3dc.stripped v3dc
start=$(date +%s)
timeout 3000 ~/amitools-venv/bin/vamos -C 68040 -H disable -m 262144 -s 4096 -- \
    v3dc -o m68k $PAIRS > m68k.txt 2> m68k.err
echo "m68k v3dc rc=$? ($(( $(date +%s) - start )) s under vamos)"
grep -vE "WARNING|^\s*$" m68k.err | grep -E "ERROR|rror" | head -5

same=0 diff=0
for f in host/*.v3dp; do
  n=$(basename $f)
  if cmp -s $f m68k/$n; then
    same=$((same + 1))
  else
    diff=$((diff + 1))
    echo "DIFFERENT: $n"; cmp -l $f m68k/$n 2>&1 | head -5
  fi
done
echo "$same identical, $diff different (of $(ls host/*.v3dp | wc -l))"
for f in host/*.v3dp; do
  python3 -I "$V/v3dp_dump.py" $f > ${f%.v3dp}.dump || echo "dump failed: $f"
done
ls -l host
