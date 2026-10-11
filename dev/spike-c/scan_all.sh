#!/bin/bash
# Scan every replayed object for the callee-saved-register bug.
# usage: scan_all.sh [toolchain prefix dir] [spike dir]
TC=${1:-$HOME/amiga-gcc162-install}
S=${2:-$HOME/spike-c162}
C="$(dirname "$(readlink -f "$0")")/check_saves.py"
OD=$TC/bin/m68k-amigaos-objdump
echo "== minimal test (/tmp/abitest/t.c)"
mkdir -p /tmp/abitest
[ -f /tmp/abitest/t.c ] && $TC/bin/m68k-amigaos-gcc -O2 -mcpu=68020 -c /tmp/abitest/t.c -o /tmp/abitest/t.o && python3 -I "$C" $OD /tmp/abitest/t.o
echo "== all objects in $S/obj"
find $S/obj -name '*.o' -print0 | xargs -0 python3 -I "$C" $OD > $S/saves_report.txt
grep -v "^suspect functions" $S/saves_report.txt | wc -l | sed 's/^/suspect functions: /'
grep -v "^suspect functions" $S/saves_report.txt | cut -d: -f1 | sort | uniq -c | sort -rn | head -8
grep -v "^suspect functions" $S/saves_report.txt | head -5
