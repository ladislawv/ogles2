#!/bin/bash
# Fills the ogles2-m6-3 package directory ($1): SDL2 (SDL2_68k with an
# ogles2.library backend, ports/sdl2) running SDL's GLES2 tests and its
# GLES2 renderer. SDL's renderer and testgles2 write shaders at run time, so
# the package brings the on-target compiler (libs/v3dglslc) and a cache
# directory (OGLES2Cache:). The library is the sync-logging build. Called by
# ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}
BS=${BS:-$HOME/ogles2-build/m68k-sync}
V=${V:-$HOME/ogles2-build/v3dglslc}

make -C "$REPO" -j8 B="$BS" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" -j8 B="$B" > /dev/null
B="$B" bash "$REPO/ports/sdl2/build.sh" > "$PKG.sdl2-build.log" 2>&1 || { tail -20 "$PKG.sdl2-build.log"; exit 1; }
rm -f "$PKG.sdl2-build.log"
mkdir -p "$PKG/libs" "$PKG/bin"
cp "$BS/ogles2.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc"
cp "$B"/sdl2/tests/* "$B/tests/rtt_churn" "$BS/cmd/OGLES2Log" "$PKG/bin/"
