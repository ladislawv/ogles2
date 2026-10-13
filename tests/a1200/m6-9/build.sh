#!/bin/bash
# Fills the ogles2-m6-9 package directory ($1): the presentation
# investigation (plan 7.2): m6_flip (no GL) and m6_vsync (ogles2's
# fullscreen vsync, with the sync-logging library). Called by
# ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k-sync}

make -C "$REPO" -j8 B="$B" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" B="$B" test-v3dp > /dev/null
mkdir -p "$PKG/bin" "$PKG/libs"
cp "$B/ogles2.library" "$PKG/libs/"
cp "$B/tests/m6_flip" "$B/tests/m6_vsync" "$PKG/bin/"
cp "$B"/v3dp/*.v3dp "$PKG/bin/"
