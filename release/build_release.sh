#!/bin/bash
# Builds the release: ~/ogles2-build/release/ogles2/ (the tree) and the
# archives ogles2-<version>.lha (users), ogles2-<version>-sdk.lha (with the
# SDK) and ogles2-<version>-sdk.zip (the SDK alone). Run inside WSL:
#   bash release/build_release.sh        (into ~/ogles2-build/release)
#   bash release/build_release.sh dist   (and copies them to release/dist/)
#   bash release/build_release.sh sdl    (also the SDL archive; with dist too)
# The SDL archive, ogles2-<version>-sdl.lha and -sdl.zip, holds SDL2 and SDL3
# with the ogles2 backend (ports/sdl2, ports/sdl3, built here), PiStorm3D's
# MiniGL SDK (SDL links it for its desktop-GL path) and two examples built
# from the SDK as assembled; it unpacks over the SDK archive.
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
PISTORM3D_SDK=${PISTORM3D_SDK:-/mnt/c/- CPP/pistorm3d/SDK}
DIST=0
SDL=0
for a in "$@"; do
    case $a in
    dist) DIST=1 ;;
    sdl) SDL=1 ;;
    *) echo "usage: build_release.sh [dist] [sdl]"; exit 1 ;;
    esac
done

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

# ---- the archives
#   ogles2-<v>.lha      for users: library, compiler, installer, examples
#   ogles2-<v>-sdk.lha  the same with SDK/
#   ogles2-<v>-sdk.zip  SDK/ alone (with the licences), for cross development on a PC
# LhA archives: lh5, which every Amiga LhA/LZX/UnArc version reads.
rm -f "$OUT"/ogles2-$VERSION*.lha "$OUT"/ogles2-$VERSION*.zip
(cd "$OUT" && $LHA -ao5q "ogles2-$VERSION-sdk.lha" ogles2)
rm -rf "$W/user" && mkdir -p "$W/user" && cp -r "$R" "$W/user/" && rm -rf "$W/user/ogles2/SDK"
(cd "$W/user" && $LHA -ao5q "$OUT/ogles2-$VERSION.lha" ogles2)
S=$W/zip/ogles2-sdk-$VERSION
rm -rf "$W/zip" && mkdir -p "$S" && cp -r "$R/SDK/." "$S/" && cp -r "$R/LICENSE" "$R/licences" "$S/"
python3 - "$W/zip" "ogles2-sdk-$VERSION" "$OUT/ogles2-$VERSION-sdk.zip" <<'EOF'
import os, sys, zipfile
base, top, out = sys.argv[1:]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, dirs, files in os.walk(os.path.join(base, top)):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, base))
EOF
(cd "$OUT" && $LHA -l "ogles2-$VERSION.lha" | tail -3)
ls -l "$OUT"/ogles2-$VERSION*.lha "$OUT"/ogles2-$VERSION*.zip
(cd "$OUT" && sha256sum ogles2-$VERSION*.lha ogles2-$VERSION*.zip)

# ---- "sdl": the SDL archive (unpacks over the SDK archive: the drawers merge)
#   ogles2-<v>-sdl.lha  ogles2/SDK/{SDL2,SDL3,MiniGL,doc/SDL.md,examples-sdl},
#                       ogles2/Examples/SDL, ogles2/licences, ogles2/ReadMe-SDL
#   ogles2-<v>-sdl.zip  the SDK part alone, under ogles2-sdk-<v>/ as the SDK zip
if [ $SDL = 1 ]; then
    B="$B" PISTORM3D_SDK="$PISTORM3D_SDK" bash "$REPO/ports/sdl2/build.sh" > "$W/sdl2-build.txt" 2>&1 ||
        { tail -20 "$W/sdl2-build.txt"; exit 1; }
    B="$B" PISTORM3D_SDK="$PISTORM3D_SDK" bash "$REPO/ports/sdl3/build.sh" > "$W/sdl3-build.txt" 2>&1 ||
        { tail -20 "$W/sdl3-build.txt"; exit 1; }
    L2=${SDL68K_SRC:-$HOME/sdl2-msn}/libSDL2-amigaos3-main
    L3=${SDL3_68K_SRC:-$HOME/sdl3-msn}/SDL3
    RS=$OUT/sdl/ogles2
    rm -rf "$OUT/sdl"
    mkdir -p "$RS"/SDK/{SDL2/include/SDL2,SDL2/lib,SDL3/include/SDL3,SDL3/lib,MiniGL,doc,examples-sdl}         "$RS/Examples/SDL" "$RS/licences"
    text "$HERE/ReadMe-SDL" "$RS/ReadMe-SDL"
    # Headers: SDL's public ones (its build configuration for AmigaOS 3 among them).
    cp "$L2"/include/*.h "$RS/SDK/SDL2/include/SDL2/"
    cp "$L3"/include/SDL3/*.h "$RS/SDK/SDL3/include/SDL3/"
    find "$RS/SDK/SDL2" "$RS/SDK/SDL3" -type f -name '*.h' -exec sed -i 's/\r$//' {} +
    cp "$B"/sdl2/libSDL2.a "$B"/sdl2/libSDL2_test.a "$RS/SDK/SDL2/lib/"
    cp "$B"/sdl3/libSDL3.a "$B"/sdl3/libSDL3_test.a "$RS/SDK/SDL3/lib/"
    # PiStorm3D's MiniGL SDK, its layout kept (its headers reach ../../../backend/).
    cp -r "$PISTORM3D_SDK/." "$RS/SDK/MiniGL/"
    text "$REPO/docs/SDL.md" "$RS/SDK/doc/SDL.md"
    for f in Makefile sdl2_sprites.c sdl3_gles2.c; do
        text "$HERE/examples-sdl/$f" "$RS/SDK/examples-sdl/$f"
    done
    sed 's/\r$//' "$L2/LICENSE" > "$RS/licences/SDL2-LICENSE.txt"
    sed 's/\r$//' "$L3/LICENSE.txt" > "$RS/licences/SDL3-LICENSE.txt"
    for f in License_MiniGL_Library.txt License_V3D_ThirdParty.txt; do
        sed 's/\r$//' "$REPO/licences/$f" > "$RS/licences/$f"
    done
    sed 's/\r$//' "$REPO/LICENSE" > "$RS/LICENSE"
    # The examples, built from the ogles2 SDK and this archive's SDK together.
    rm -rf "$W/sdk-sdl" && mkdir -p "$W/sdk-sdl" && cp -r "$R/SDK/." "$RS/SDK/." "$W/sdk-sdl/"
    make -C "$W/sdk-sdl/examples-sdl" CC="$TC/bin/m68k-amigaos-gcc" > "$W/examples-sdl.txt" 2>&1 ||
        { grep -v lang_reloc "$W/examples-sdl.txt" | tail -20; exit 1; } # the linker traces relocations on stderr
    for p in sdl2_sprites sdl3_gles2; do
        $TC/bin/m68k-amigaos-strip -o "$RS/Examples/SDL/$p" "$W/sdk-sdl/examples-sdl/$p"
    done
    echo "SDL examples: built"
    rm -f "$OUT"/ogles2-$VERSION-sdl.lha "$OUT"/ogles2-$VERSION-sdl.zip
    (cd "$OUT/sdl" && $LHA -ao5q "$OUT/ogles2-$VERSION-sdl.lha" ogles2)
    SZ=$W/zip-sdl/ogles2-sdk-$VERSION
    rm -rf "$W/zip-sdl" && mkdir -p "$SZ" && cp -r "$RS/SDK/." "$SZ/" && cp -r "$RS/LICENSE" "$RS/licences" "$SZ/"
    text "$HERE/ReadMe-SDL" "$SZ/ReadMe-SDL"
    python3 - "$W/zip-sdl" "ogles2-sdk-$VERSION" "$OUT/ogles2-$VERSION-sdl.zip" <<'PYEOF'
import os, sys, zipfile
base, top, out = sys.argv[1:]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, dirs, files in os.walk(os.path.join(base, top)):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, base))
PYEOF
    ls -l "$OUT"/ogles2-$VERSION-sdl.lha "$OUT"/ogles2-$VERSION-sdl.zip
fi

# ---- "dist": the archives into the repository (release/dist/), for those
# who develop on Windows without WSL's build output at hand. Only on request:
# each copy is a new 10 MB in the repository's history.
if [ $DIST = 1 ]; then
    rm -f "$HERE"/dist/ogles2-*.lha "$HERE"/dist/ogles2-*.zip
    mkdir -p "$HERE/dist"
    cp "$OUT"/ogles2-$VERSION*.lha "$OUT"/ogles2-$VERSION*.zip "$HERE/dist/"
    (cd "$HERE/dist" && sha256sum ogles2-$VERSION*.lha ogles2-$VERSION*.zip > SHA256SUMS)
    echo "copied to $HERE/dist"
fi
