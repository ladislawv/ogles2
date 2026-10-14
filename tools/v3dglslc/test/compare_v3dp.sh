#!/bin/bash
# Write .v3dp files for the test programs with the native x86-64 v3dglslc
# (build_host.sh) and the m68k v3dglslc under vamos (build_m68k.sh), and
# require them to be identical byte for byte. Then decode the host files with
# the independent reader (v3dp/v3dp_dump.py).
#   usage: compare_v3dp.sh
set -u
OUT=${OUT:-$HOME/ogles2-build/v3dglslc}
T="$(dirname "$(readlink -f "$0")")"
V="$(dirname "$(dirname "$(dirname "$T")")")/v3dp"
W=$OUT/v3dpcmp && rm -rf $W && mkdir -p $W/host $W/m68k && cd $W
cp "$T"/shaders/*.vert "$T"/shaders/*.frag . || exit 1
# The source hash covers the exact text: LF line ends, as in the repository
# and the A1200 packages (a Windows checkout must not turn them into CRLF).
sed -i 's/\r$//' *.vert *.frag
PAIRS="tex.vert tex.frag light.vert light.frag skin.vert skin.frag tex.vert proc.frag state.vert state.frag mrt.vert mrt.frag"

$OUT/host/v3dglslc -o host $PAIRS > host.txt 2>&1
echo "host v3dglslc rc=$?"

cp $OUT/v3dglslc.stripped v3dglslc
start=$(date +%s)
timeout 3000 ~/amitools-venv/bin/vamos -C 68040 -H disable -m 262144 -s 4096 -- \
    v3dglslc -o m68k $PAIRS > m68k.txt 2> m68k.err
echo "m68k v3dglslc rc=$? ($(( $(date +%s) - start )) s under vamos)"
grep -vE "WARNING|^\s*$" m68k.err | grep -E "ERROR|rror" | head -5

# The programs' files and, for those with samplers, their 32-bit-sampler
# variants (.v3dw).
same=0 diff=0
for f in host/*.v3dp host/*.v3dw; do
  [ -e "$f" ] || continue
  n=$(basename $f)
  if cmp -s $f m68k/$n; then
    same=$((same + 1))
  else
    diff=$((diff + 1))
    echo "DIFFERENT: $n"; cmp -l $f m68k/$n 2>&1 | head -5
  fi
done
echo "$same identical, $diff different (of $(ls host/*.v3dp host/*.v3dw 2>/dev/null | wc -l))"
for f in host/*.v3dp host/*.v3dw; do
  [ -e "$f" ] || continue
  python3 -I "$V/v3dp_dump.py" $f > $f.dump || echo "dump failed: $f"
done
