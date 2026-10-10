#!/bin/bash
# Build v3dglslc for m68k-amigaos (GCC 16.2, libnix, software floating point).
#   usage: build_m68k.sh        -> $OUT/v3dglslc (and v3dglslc.stripped)
# Needs the Mesa objects first: replay.py (default --libs tool).
set -u
OUT=${OUT:-$HOME/ogles2-build/v3dglslc}
TC=${M68K_TC:-$HOME/amiga-gcc162-install}
P=$TC/bin/m68k-amigaos-
H="$(dirname "$(readlink -f "$0")")"
V="$(dirname "$(dirname "$H")")/v3dp"
A=$OUT/lib
mkdir -p $A $OUT/main

# ---- compile v3dglslc.c and the .v3dp writer with the flags Mesa uses for st_manager.c
python3 -I - "$H" "$OUT" "$V" <<'PY'
import json, os, subprocess, sys
h, out, v = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, h)
import replay
cc = json.load(open(os.path.join(replay.MESA_BUILD, "compile_commands.json")))
ref = next(e for e in cc if e["file"].endswith("state_tracker/st_manager.c"))
commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=replay.MESA_SRC,
                        capture_output=True, text=True).stdout.strip()[:12]
rc = 0
for src in (os.path.join(h, "v3dglslc.c"), os.path.join(v, "v3dp_writer.c")):
    cmd, obj, rel = replay.rewrite(ref, out, [])
    o = os.path.join(out, "main", os.path.basename(src)[:-2] + ".o")
    cmd = [src if c == ref["file"] else (o if c == obj else c) for c in cmd]
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

# ---- archives of everything replay.py compiled, then link
rm -f $A/*.a
O=$OUT/obj
for d in $(find $O -type d -name '*.a.p') $O/compat; do
  name=$(basename "$d" .a.p); name=${name#lib}
  find "$d" -name '*.o' | sort > $A/$name.list
  [ -s $A/$name.list ] && ${P}ar rcs $A/lib$name.a $(cat $A/$name.list)
done
LIBS=$(ls $A/*.a | sed -E 's|.*/lib(.*)\.a|-l\1|' | tr '\n' ' ')
${P}g++ -mcpu=68020 -msoft-float -mcrt=nix20 -fno-exceptions -fno-rtti -o $OUT/v3dglslc \
    $OUT/main/v3dglslc.o $OUT/main/v3dp_writer.o \
    -L$A -Wl,--start-group $LIBS -Wl,--end-group -lstdc++ -lpthread -latomic -lm \
    -Wl,--allow-multiple-definition -Wl,-Map,$OUT/v3dglslc.map > $OUT/link.log 2>&1
echo "m68k v3dglslc link rc=$?"
grep -oE "undefined reference to \`[^']+'" $OUT/link.log | sed "s/undefined reference to \`//; s/'$//" \
    | sort | uniq -c | sort -rn > $OUT/undefined.txt
echo "undefined symbols: $(wc -l < $OUT/undefined.txt)"
head -40 $OUT/undefined.txt
[ -f $OUT/v3dglslc ] && ${P}strip -o $OUT/v3dglslc.stripped $OUT/v3dglslc && ls -l $OUT/v3dglslc $OUT/v3dglslc.stripped
