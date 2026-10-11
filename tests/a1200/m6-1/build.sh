#!/bin/bash
# Fills the ogles2-m6-1 package directory ($1): gl4es (desktop OpenGL on
# GLES2, ports/gl4es) drawing the gears comparison's scene. gl4es writes its
# shaders at run time, so the package brings the on-target compiler
# (libs/v3dglslc) and a cache directory (OGLES2Cache:). The library is the
# sync-logging build: a first port may hang. Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}
BS=${BS:-$HOME/ogles2-build/m68k-sync}
V=${V:-$HOME/ogles2-build/v3dglslc}

make -C "$REPO" -j8 B="$BS" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" -j8 B="$B" > /dev/null
make -C "$REPO" B="$B" gl4es > /dev/null 2>&1
mkdir -p "$PKG/libs" "$PKG/bin"
cp "$BS/ogles2.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc"
cp "$B/tests/gears_gl4es" "$BS/cmd/OGLES2Log" "$PKG/bin/"
