#!/bin/bash
# Build v3dglslc natively (x86-64), the offline compiler (S1): the same
# program, writer and fake DRM device as the m68k build, linked against the
# host Mesa build's own objects (same commit and patches). Its .v3dp files are
# the reference the m68k ones must match byte for byte (test/compare_v3dp.sh).
#   usage: build_host.sh            -> $OUT/host/v3dglslc
set -u
OUT=${OUT:-$HOME/ogles2-build/v3dglslc}
H="$(dirname "$(readlink -f "$0")")"
V="$(dirname "$(dirname "$H")")/v3dp"
M=~/mesa-build/mesa
B=$M/build
W=$OUT/host
mkdir -p $W/lib && rm -f $W/lib/*.a

# Bring the host objects up to date (patches/ change Mesa sources).
ninja -C $B > $W/ninja.log 2>&1 || { echo "ninja failed, see $W/ninja.log"; tail -20 $W/ninja.log; exit 1; }

# ---- compile v3dglslc.c, the writer and the fake device with st_manager.c's host flags
python3 -I - "$H" "$W" "$V" "$H/compat" <<'PY'
import json, os, shlex, subprocess, sys
h, w, v, compat = sys.argv[1:5]
build = os.path.expanduser("~/mesa-build/mesa/build")
src_root = os.path.dirname(build)
cc = json.load(open(os.path.join(build, "compile_commands.json")))
ref = next(e for e in cc if e["file"].endswith("state_tracker/st_manager.c"))
base = shlex.split(ref["command"])
commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=src_root,
                        capture_output=True, text=True).stdout.strip()[:12]
rc = 0
for src in (os.path.join(h, "v3dglslc.c"), os.path.join(v, "v3dp_writer.c"),
            os.path.join(compat, "fake_drm.c")):
    out = os.path.join(w, os.path.basename(src)[:-2] + ".o")
    cmd, skip = [], False
    for c in base:
        if skip:
            skip = False
            continue
        if c in ("-o", "-MF", "-MQ"):
            skip = True
            continue
        if c in ("-c", "-MD") or c == ref["file"] or c.endswith("st_manager.c"):
            continue
        cmd.append(c)
    cmd[1:1] = ["-I" + os.path.join(src_root, "src/gallium/drivers/v3d"),
                "-I" + os.path.join(build, "src/gallium/drivers/v3d"),
                "-I" + os.path.join(src_root, "src/gallium/auxiliary/pipe-loader"),
                "-I" + os.path.join(src_root, "src/broadcom"),
                "-I" + os.path.join(build, "src/broadcom"),
                "-I" + os.path.join(src_root, "include"),
                "-I" + h, "-I" + v, '-DV3DP_MESA_COMMIT="%s"' % commit]
    cmd += ["-c", src, "-o", out]
    p = subprocess.run(cmd, cwd=build, capture_output=True, text=True)
    print("%s compile rc=%d" % (os.path.basename(src), p.returncode))
    print("\n".join(l for l in p.stderr.splitlines() if "error" in l or "warning" in l)[:3000])
    rc = rc or p.returncode
sys.exit(rc)
PY
[ $? -eq 0 ] || exit 1

# ---- archives of the same Mesa targets the m68k build uses (replay.py TOOL)
DIRS=$(python3 -I -c "
import sys; sys.path.insert(0, '$H'); import replay
print(' '.join(replay.LIBS[n] for n in replay.TOOL))")
# x86-only helpers the m68k build replaces (compat/driver_stubs.c, plain C paths)
DIRS="$DIRS src/mesa/libmesa_sse41.a.p src/util/libmesa_util_simd.a.p"
for d in $DIRS; do
  name=$(basename "$d" .a.p); name=${name#lib}
  find "$B/$d" -name '*.o' | sort > $W/lib/$name.list
  [ -s $W/lib/$name.list ] && ar rcs $W/lib/lib$name.a $(cat $W/lib/$name.list)
done
LIBS=$(ls $W/lib/*.a | sed -E 's|.*/lib(.*)\.a|-l\1|' | tr '\n' ' ')
LLVM=$(llvm-config --ldflags --libs 2>/dev/null || echo -lLLVM)
g++ -o $W/v3dglslc $W/v3dglslc.o $W/v3dp_writer.o $W/fake_drm.o \
    -L$W/lib -Wl,--start-group $LIBS -Wl,--end-group \
    $LLVM -lSPIRV-Tools-opt -lSPIRV-Tools -lSPIRV-Tools-link \
    -lexpat -lzstd -lz -ldrm -lpthread -ldl -lm > $W/link.log 2>&1
echo "host v3dglslc link rc=$?"
grep -oE "undefined reference to \`[^']+'" $W/link.log | sed "s/undefined reference to \`//; s/'$//" \
    | sort | uniq -c | sort -rn | head -30
ls -l $W/v3dglslc 2>/dev/null
