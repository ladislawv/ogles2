#!/bin/bash
# Fills the ogles2-m4-2 package directory ($1): the gears speed comparison
# (tests/perf), ogles2.library against PiStorm3D 29.1's minigl.library.
# Called by ../build_package.sh. The library is the normal build (the
# sync-logging one would slow it down); minigl.library and its SDK come from
# PiStorm3D 29.1 (Aminet util/libs/PiStorm3D.lha) in $MGL_DIR.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}
MGL_DIR=${MGL_DIR:-$HOME/minigl-29.1}

make -C "$REPO" -j8 B="$B" > /dev/null
make -C "$REPO" B="$B" MGL_SDK="$MGL_DIR/SDK" perf-mgl > /dev/null
make -C "$REPO" B="$B" test-v3dp > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin"
cp "$B/ogles2.library" "$MGL_DIR/bin/minigl.library" "$PKG/libs/"
cp "$B/tests/gears_gles2" "$B/tests/gears_mgl" "$PKG/bin/"
cp "$B"/v3dp/*.v3dp "$PKG/bin/"
