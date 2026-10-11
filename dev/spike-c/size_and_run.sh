#!/bin/bash
# Size breakdown of the linked m68k glsl_compiler, then a functional run under vamos.
S=${S:-$HOME/spike-c162}
P=${M68K_TC:-$HOME/amiga-gcc162-install}/bin/m68k-amigaos-
cd $S || exit 1
${P}strip -o glsl_compiler.stripped glsl_compiler
echo "unstripped $(stat -c %s glsl_compiler) bytes, stripped $(stat -c %s glsl_compiler.stripped) bytes"
${P}size glsl_compiler | tail -1

# Code+data per library, from the link map (input sections actually linked).
python3 - "$S/glsl_compiler.map" <<'PY'
import re, sys, collections
tot = collections.Counter()
cur = None
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"^\s*(\.(?:text|data|rodata|bss)\S*)?\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S+)$", line)
    if not m:
        continue
    size = int(m.group(2), 16)
    src = m.group(3)
    if "/spike-c/lib/lib" in src:
        key = src.split("/spike-c/lib/lib")[1].split(".a(")[0]
    elif "libstdc++" in src:
        key = "libstdc++"
    elif "libnix" in src or "/libm" in src:
        key = "libnix/libm"
    elif "glsl_compiler.p" in src:
        key = "main"
    else:
        key = "other (" + src.rsplit("/", 1)[-1].split("(")[0] + ")"
    tot[key] += size
allsum = sum(tot.values())
for k, v in tot.most_common(20):
    print(f"{v/1024:9.1f} KB  {k}")
print(f"{allsum/1024:9.1f} KB  total input sections")
PY

# Functional test under vamos (68020 emulation, no timing value).
mkdir -p $S/run && cd $S/run
cat > t.frag <<'EOF'
#version 100
precision mediump float;
uniform sampler2D tex;
uniform vec4 tint;
varying vec2 uv;
void main()
{
    vec4 c = texture2D(tex, uv) * tint;
    if (c.a < 0.5)
        discard;
    gl_FragColor = vec4(pow(c.rgb, vec3(1.0 / 2.2)), c.a);
}
EOF
cat > t.vert <<'EOF'
#version 100
attribute vec4 pos;
attribute vec2 tc;
uniform mat4 mvp;
varying vec2 uv;
void main()
{
    uv = tc;
    gl_Position = mvp * pos;
}
EOF
cp $S/glsl_compiler.stripped glsl_compiler
for args in "--version 100 --dump-hir t.frag" "--version 100 --link t.vert t.frag"; do
  echo "== vamos glsl_compiler $args"
  timeout 600 ~/amitools-venv/bin/vamos -C 68040 -H disable -m 262144 -s 2048 -- glsl_compiler $args > out.txt 2> err.txt
  echo "rc=$?"
  head -c 1500 out.txt; echo
  grep -vE "WARNING|^\s*$" err.txt | head -8
done
