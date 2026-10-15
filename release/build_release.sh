#!/bin/bash
# Builds the release archive: ~/ogles2-build/release/ogles2/ (the tree) and
# ~/ogles2-build/release/ogles2-<version>.lha. Run inside WSL:
#   bash release/build_release.sh
#
# It builds the library, the import libraries (GCC and vbcc) and OGLES2Log,
# compiles the examples' shaders with the offline compiler (host v3dglslc)
# and the examples themselves with the SDK's own Makefile, from the SDK as
# assembled, so the SDK is what is tested. The on-target compiler
# (v3dglslc.stripped) must be the build of the same Mesa as the host one:
# tools/v3dglslc/test/compare_v3dp.sh checks that.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
HERE=$REPO/release
B=${B:-$HOME/ogles2-build/m68k}
V=${V:-$HOME/ogles2-build/v3dglslc}
TC=${TC:-$HOME/amiga-gcc-install}
VBCC=${VBCC:-$HOME/vbcc}
MESA=${MESA:-$HOME/mesa-build/mesa}
LHA=${LHA:-$HOME/tools/bin/lha}
OUT=${OUT:-$HOME/ogles2-build/release}

VER=$(sed -n 's/^#define OGLES2_LIB_VERSION *\([0-9]*\).*/\1/p' "$REPO/ogles2/ogles2_internal.h")
REV=$(sed -n 's/^#define OGLES2_LIB_REVISION *\([0-9]*\).*/\1/p' "$REPO/ogles2/ogles2_internal.h")
VERSION=$VER.$REV
R=$OUT/ogles2
echo "ogles2 $VERSION -> $R"

# ---- builds
make -C "$REPO" -j8 B="$B" > /dev/null
make -C "$REPO" B="$B" vbcc > /dev/null
[ -f "$V/v3dglslc.stripped" ] && [ -x "$V/host/v3dglslc" ] || {
    echo "needs $V/v3dglslc.stripped and $V/host/v3dglslc (tools/v3dglslc/build_m68k.sh, build_host.sh)"
    exit 1
}

rm -rf "$R" "$OUT/ogles2-$VERSION.lha" "$OUT/work"
mkdir -p "$R"/{Libs,C,Examples,SDK/include,SDK/lib,SDK/doc,SDK/examples,licences/Mesa} "$OUT/work"

# Text files: LF line ends and ASCII, as AmigaOS's tools expect.
text() { # source destination
    sed 's/\r$//' "$1" > "$2"
    if LC_ALL=C grep -nP '[^\x00-\x7F]' "$2"; then echo "non-ASCII in $1"; exit 1; fi
}

# ---- the user's part
text "$HERE/ReadMe" "$R/ReadMe"
text "$HERE/Install-ogles2" "$R/Install-ogles2"
cp "$B/ogles2.library" "$R/Libs/"
cp "$V/v3dglslc.stripped" "$R/Libs/v3dglslc"
cp "$B/cmd/OGLES2Log" "$R/C/"
$TC/bin/m68k-amigaos-strip "$R/C/OGLES2Log"

# ---- licences (LICENSE and the Khronos/third-party notices may hold UTF-8)
sed 's/\r$//' "$REPO/LICENSE" > "$R/LICENSE"
for f in "$REPO"/licences/*; do sed 's/\r$//' "$f" > "$R/licences/$(basename "$f")"; done
cp "$MESA/docs/license.rst" "$R/licences/Mesa/license.txt"
for l in MIT Apache-2.0 BSL-1.0; do cp "$MESA/licenses/$l" "$R/licences/Mesa/$l.txt"; done
cat > "$R/licences/Mesa/ReadMe" <<'EOF'
LIBS:v3dglslc is Mesa's GLSL compiler and V3D back end (Mesa 74b6971 with the
patches in the ogles2 source, tools/v3dglslc/patches). The Mesa files it is
built from are MIT (MIT.txt; license.txt is Mesa's own summary); header-only
parts of Mesa's util library carry Apache-2.0 or BSL-1.0 alternatives, whose
texts are here too.
EOF

# ---- SDK: headers (the repository's and the generated ones), import libraries
cp -r "$REPO/include/." "$R/SDK/include/"
cp -r "$B/gen/include/." "$R/SDK/include/"
find "$R/SDK/include" -type f -name '*.h' -exec sed -i 's/\r$//' {} +
cp "$B/libogles2.a" "$R/SDK/lib/"
cp "$B/vbcc/ogles2.lib" "$R/SDK/lib/"
# Autodoc: a form feed before each entry
sed 's/\r$//' "$HERE/ogles2.doc" | awk 'NR > 1 && /^ogles2\.library\/[^ ]+ +ogles2\.library\// {printf "\f"} {print}' \
    > "$R/SDK/doc/ogles2.doc"
text "$HERE/GettingStarted.txt" "$R/SDK/doc/GettingStarted.txt"
for f in Makefile triangle.c triangle_shaders.h cube.c cube_shaders.h; do
    text "$HERE/examples/$f" "$R/SDK/examples/$f"
done

# ---- the examples' shader binaries: the GLSL byte for byte as the programs
# pass it, compiled by the offline compiler, named by hash
W=$OUT/work
gcc -O2 -o "$W/emit" "$HERE/emit_shaders.c"
mkdir -p "$W/src" "$W/v3dp"
(cd "$W/src" && ../emit)
for p in triangle cube; do
    "$V/host/v3dglslc" -o "$W/v3dp" "$W/src/$p.vert" "$W/src/$p.frag" > "$W/v3dglslc-$p.txt" 2>&1 || {
        cat "$W/v3dglslc-$p.txt"; exit 1; }
done
python3 "$REPO/v3dp/v3dp_hashname.py" "$W"/v3dp/*.v3dp
cp "$W"/v3dp/*.v3dp "$R/Examples/"

# ---- the examples, built from the SDK as shipped (GCC; vbcc to check)
cp -r "$R/SDK/." "$W/sdk/"
make -C "$W/sdk/examples" CC="$TC/bin/m68k-amigaos-gcc" > "$W/examples-gcc.txt"
for p in triangle cube; do
    $TC/bin/m68k-amigaos-strip -o "$R/Examples/$p" "$W/sdk/examples/$p"
done
mkdir -p "$W/sdk-vbcc" && cp -r "$R/SDK/." "$W/sdk-vbcc/"
PATH=$VBCC/bin:$PATH VBCC=$VBCC make -C "$W/sdk-vbcc/examples" vbcc \
    VC="$VBCC/bin/vc +aos68k" VCFLAGS="-c99 -cpu=68020 -fpu=68881 -O1 -I$TC/m68k-amigaos/ndk-include" \
    > "$W/examples-vbcc.txt"
echo "examples: GCC and vbcc builds ok"

# ---- the archive: lh5, which every Amiga LhA/LZX/UnArc version reads
(cd "$OUT" && $LHA -ao5q "ogles2-$VERSION.lha" ogles2)
(cd "$OUT" && $LHA -l "ogles2-$VERSION.lha")
ls -l "$OUT/ogles2-$VERSION.lha"
sha256sum "$OUT/ogles2-$VERSION.lha"
