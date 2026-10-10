#!/bin/bash
# Fills the ogles2-m2-1 package directory ($1). Called by ../build_package.sh.
# The library is the sync-logging build: every log line reaches the file at
# once, so a GPU hang loses nothing. The programs' binaries go next to the
# tests (PROGDIR:), their GLSL into shaders/ (m2_link reads it).
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k-sync}

make -C "$REPO" -j8 B="$B" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" B="$B" test-v3dp > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin" "$PKG/shaders"
cp "$B/ogles2.library" "$PKG/libs/"
cp "$B/tests/m1_draw" "$B/tests/m1_soak" "$B/tests/m2_link" "$B/cmd/OGLES2Log" "$PKG/bin/"
cp "$B"/v3dp/*.v3dp "$PKG/bin/"
cp "$B"/v3dp/src/*.vert "$B"/v3dp/src/*.frag "$PKG/shaders/"
