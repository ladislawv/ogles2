#!/bin/bash
# Fills the ogles2-m6-6 package directory ($1). Called by ../build_package.sh.
# The library is the sync-logging build: every log line reaches the file at
# once, so a GPU hang loses nothing. The programs' binaries (.v3dp, and the
# 32-bit-sampler variants, .v3dw) go next to the tests (PROGDIR:), their GLSL
# into shaders/ (m2_link reads it). bin2/ holds the extension test without
# binaries, and libs/ the on-target compiler: step 5 compiles everything on
# the Amiga, the .v3dw files when a draw first needs them.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k-sync}
V=${V:-$HOME/ogles2-build/v3dglslc}

make -C "$REPO" -j8 B="$B" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" B="$B" test-v3dp > /dev/null
mkdir -p "$PKG/libs" "$PKG/compiler" "$PKG/bin" "$PKG/bin2" "$PKG/shaders"
cp "$B/ogles2.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/compiler/v3dglslc" # in LIBS: for step 5 only
cp "$B/tests/m6_ext" "$B/tests/m1_draw" "$B/tests/m2_texture" "$B/tests/m2_link" "$B/tests/m2_elements" \
   "$B/tests/m2_buffers" "$B/tests/m4_depth" "$B/tests/m5_fbo" "$B/cmd/OGLES2Log" "$PKG/bin/"
cp "$B/tests/m6_ext" "$PKG/bin2/"
cp "$B"/v3dp/*.v3dp "$B"/v3dp/*.v3dw "$PKG/bin/"
cp "$B"/v3dp/src/*.vert "$B"/v3dp/src/*.frag "$PKG/shaders/"
