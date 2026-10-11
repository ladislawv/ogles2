#!/bin/bash
# Largest m68k objects (text+data) next to the same objects from the x86-64 host build.
S=${S:-$HOME/spike-c}
P=~/amiga-gcc16-install/bin/m68k-amigaos-
B=~/mesa-build/mesa/build
find $S/obj -name '*.o' -print0 | xargs -0 ${P}size 2>/dev/null | awk 'NR>1 {print $1+$2, $6}' | sort -rn | head -15 > $S/top.txt
printf "%10s %10s  %s\n" "m68k KB" "x86-64 KB" "object"
while read sz path; do
  rel=${path#$S/obj/}
  host=$B/$rel
  hs=$( [ -f "$host" ] && size "$host" | awk 'NR==2 {print $1+$2}' )
  printf "%10.1f %10s  %s\n" "$(echo "$sz/1024" | bc -l)" "$( [ -n "$hs" ] && printf %.1f "$(echo "$hs/1024" | bc -l)" || echo -)" "$rel"
done < $S/top.txt
echo "== totals (all replayed objects)"
echo "m68k: $(find $S/obj -name '*.o' -print0 | xargs -0 ${P}size 2>/dev/null | awk 'NR>1 {s+=$1+$2} END {printf "%.1f MB", s/1048576}')"
