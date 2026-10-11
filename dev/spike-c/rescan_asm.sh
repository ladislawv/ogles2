#!/bin/bash
# Re-run check_saves_asm.py on assembly already produced by scan_asm.sh.
C="$(dirname "$(readlink -f "$0")")/check_saves_asm.py"
cd /tmp/abitest 2>/dev/null && for f in t_*.s; do echo "minimal $f: $(python3 -I "$C" "$f" | tail -1)"; done
for n in gcc16.1 gcc16.2; do
  find ~/spike-asm-$n/obj -name '*.o' -print0 | xargs -0 python3 -I "$C" > ~/spike-asm-$n/report.txt
  echo "== $n Mesa sources: $(tail -1 ~/spike-asm-$n/report.txt)"
  grep -v "^suspect" ~/spike-asm-$n/report.txt | head -6
done
echo "== gcc6.5.0b PiStorm3D driver: $(python3 -I "$C" ~/spike-asm-gcc6/*.s | tail -1)"
