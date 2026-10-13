#!/bin/bash
# Fills the ogles2-m6-7 package directory ($1): glmark2-es2 (ports/glmark2)
# with its data, the sync-logging library and the on-target shader compiler
# (glmark2's shaders compile on the Amiga when first used and are kept in
# cache/). Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}
BS=${BS:-$HOME/ogles2-build/m68k-sync}
V=${V:-$HOME/ogles2-build/v3dglslc}

make -C "$REPO" -j8 B="$BS" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" -j8 B="$B" > /dev/null
B="$B" bash "$REPO/ports/glmark2/build.sh" > /dev/null 2>&1 || { echo "ports/glmark2/build.sh failed"; exit 1; }
mkdir -p "$PKG/libs" "$PKG/bin" "$PKG/cache"
cp "$BS/ogles2.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc"
cp "$B/glmark2/glmark2-es2" "$BS/cmd/OGLES2Log" "$PKG/bin/"
cp -r "$B/glmark2/data" "$PKG/bin/data"
