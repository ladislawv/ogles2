#!/bin/bash
# Fills the ogles2-m1-4 package directory ($1). Called by ../build_package.sh.
# The library is the sync-logging build: every log line reaches the file at
# once, so a GPU hang loses nothing.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k-sync}

make -C "$REPO" -j8 B="$B" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin"
cp "$B/ogles2.library" "$PKG/libs/"
cp "$B/tests/m1_probe" "$B/tests/m1_clear" "$B/cmd/OGLES2Log" "$PKG/bin/"
