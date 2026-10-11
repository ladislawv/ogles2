#!/bin/bash
# Spike part 2: build v3dc (m68k) and s1ref (x86-64), run both on the test
# shaders and compare the V3D QPU code they print.
#   usage: build_v3dc.sh            (after replay.py --libs all)
set -u
S=${S:-$HOME/spike-c162}
TC=${M68K_TC:-$HOME/amiga-gcc162-install}
P=$TC/bin/m68k-amigaos-
H="$(dirname "$(readlink -f "$0")")"
D="$(dirname "$H")"
M=~/mesa-build/mesa
B=$M/build
A=$S/lib2
mkdir -p $A $S/v3dc

# ---- m68k: compile v3dc.c and the .v3dp writer with the flags Mesa uses for st_manager.c
python3 -I - "$H" "$S" "$D/../v3dp" <<'PY'
import json, os, shlex, subprocess, sys
h, s, v = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, os.path.dirname(h))
import replay
cc = json.load(open(os.path.join(replay.MESA_BUILD, "compile_commands.json")))
ref = next(e for e in cc if e["file"].endswith("state_tracker/st_manager.c"))
commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=replay.MESA_SRC,
                        capture_output=True, text=True).stdout.strip()[:12]
rc = 0
for src in (os.path.join(h, "v3dc.c"), os.path.join(v, "v3dp_writer.c")):
    cmd, obj, rel = replay.rewrite(ref, s, [])
    out = os.path.join(s, "v3dc", os.path.basename(src)[:-2] + ".o")
    cmd = [src if c == ref["file"] else (out if c == obj else c) for c in cmd]
    cmd[1:1] = ["-I" + os.path.join(replay.MESA_SRC, "src/gallium/drivers/v3d"),
                "-I" + os.path.join(replay.MESA_BUILD, "src/gallium/drivers/v3d"),
                "-I" + os.path.join(replay.MESA_SRC, "src/gallium/auxiliary/pipe-loader"),
                "-I" + os.path.join(replay.MESA_SRC, "src/broadcom"),
                "-I" + os.path.join(replay.MESA_BUILD, "src/broadcom"),
                "-I" + h, "-I" + v, '-DV3DP_MESA_COMMIT="%s"' % commit]
    p = subprocess.run(cmd, cwd=replay.MESA_BUILD, capture_output=True, text=True)
    print("%s compile rc=%d" % (os.path.basename(src), p.returncode))
    print("\n".join(l for l in p.stderr.splitlines() if "error" in l or "warning" in l)[:3000])
    rc = rc or p.returncode
sys.exit(rc)
PY
[ $? -eq 0 ] || exit 1

# ---- m68k: archives of everything compiled, then link
rm -f $A/*.a
O=$S/obj
for d in $(find $O -type d -name '*.a.p') $O/compat; do
  name=$(basename "$d" .a.p); name=${name#lib}
  find "$d" -name '*.o' | sort > $A/$name.list
  [ -s $A/$name.list ] && ${P}ar rcs $A/lib$name.a $(cat $A/$name.list)
done
LIBS=$(ls $A/*.a | sed -E 's|.*/lib(.*)\.a|-l\1|' | tr '\n' ' ')
${P}g++ -mcpu=68020 -msoft-float -mcrt=nix20 -fno-exceptions -fno-rtti -o $S/v3dc_bin $S/v3dc/v3dc.o $S/v3dc/v3dp_writer.o \
    -L$A -Wl,--start-group $LIBS -Wl,--end-group -lstdc++ -lpthread -latomic -lm \
    -Wl,--allow-multiple-definition -Wl,-Map,$S/v3dc.map > $S/v3dc_link.log 2>&1
echo "v3dc link rc=$?"
grep -oE "undefined reference to \`[^']+'" $S/v3dc_link.log | sed "s/undefined reference to \`//; s/'$//" \
    | sort | uniq -c | sort -rn > $S/v3dc_undefined.txt
echo "v3dc undefined symbols: $(wc -l < $S/v3dc_undefined.txt)"
head -40 $S/v3dc_undefined.txt
[ -f $S/v3dc_bin ] && ${P}strip -o $S/v3dc.stripped $S/v3dc_bin && ls -l $S/v3dc_bin $S/v3dc.stripped
