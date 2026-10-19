#!/bin/bash
# Fills the ogles2-m7-6 package directory ($1): glCopyTexImage2D and
# glCopyTexSubImage2D (m7_copy, GitHub issue 5): copies from a bitmap's, a
# window's and a framebuffer object's pictures checked, then the frame time
# with a whole-screen copy (OpenLara's refraction), drawn by the GPU and
# (probe K) through the CPU. The normal library, which the benchmark
# measures; SYNC=1 takes the sync-logging one (every log line reaches the
# file at once, so a hang loses nothing). Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
if [ "${SYNC:-0}" = 1 ]; then
    B=${B:-$HOME/ogles2-build/m68k-sync}
    make -C "$REPO" -j8 B="$B" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
else
    B=${B:-$HOME/ogles2-build/m68k}
    make -C "$REPO" -j8 B="$B" > /dev/null
fi
make -C "$REPO" B="$B" test-v3dp > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin"
cp "$B/ogles2.library" "$PKG/libs/"
cp "$B/tests/m7_copy" "$B/cmd/OGLES2Log" "$PKG/bin/"
cp "$B"/v3dp/*.v3dp "$B"/v3dp/*.v3dw "$PKG/bin/"
