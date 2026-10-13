#!/bin/bash
# Fills the ogles2-m6-10 package directory ($1): 4x MSAA (m6_msaa), the
# earlier drawing, texture and framebuffer tests, and the same three with
# ENV:OGLES2/MSAA=4 (every context of theirs multisampled). The
# sync-logging library: every log line reaches the file at once, so a GPU
# hang loses nothing. Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k-sync}

make -C "$REPO" -j8 B="$B" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" B="$B" test-v3dp > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin"
cp "$B/ogles2.library" "$PKG/libs/"
cp "$B/tests/m6_msaa" "$B/tests/m1_draw" "$B/tests/m2_texture" "$B/tests/m5_fbo" "$B/cmd/OGLES2Log" "$PKG/bin/"
cp "$B"/v3dp/*.v3dp "$B"/v3dp/*.v3dw "$PKG/bin/"
