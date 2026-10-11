#!/bin/bash
# Compare the .v3dp files the A1200 wrote (a1200/v3dp-1 package, results/v3dp)
# with the x86-64 ones from compare_v3dp.sh, byte for byte.
#   usage: check_a1200_v3dp.sh <results dir>
set -u
R=${1:?results directory}
S=${S:-$HOME/spike-c162}
REF=$S/v3dpcmp/host
V="$(dirname "$(dirname "$(dirname "$(readlink -f "$0")")")")/v3dp"
[ -d "$REF" ] || { echo "run compare_v3dp.sh first"; exit 1; }
same=0 diff=0
for f in $REF/*.v3dp; do
  n=$(basename $f)
  a=$(find "$R" -iname "$n" | head -1)
  if [ -z "$a" ]; then
    echo "MISSING: $n"; diff=$((diff + 1))
  elif cmp -s "$f" "$a"; then
    same=$((same + 1))
  else
    echo "DIFFERENT: $n"; cmp -l "$f" "$a" 2>&1 | head -5; diff=$((diff + 1))
    python3 -I "$V/v3dp_dump.py" "$a" > /dev/null || true
  fi
done
echo "$same identical, $diff different or missing (of $(ls $REF/*.v3dp | wc -l))"
