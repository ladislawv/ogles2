#!/bin/bash
# Fills the ogles2-m6-5 package directory ($1): Quake III Arena with
# ioquake3's GLES2 renderer on ogles2.library directly (ports/ioq3-gl2, no
# gl4es), the on-target shader compiler, and a shader cache. The game data
# stays where it is installed on the A1200 (Run-M1's Q3DATA).
# Called by ../build_package.sh.
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
B="$B" bash "$REPO/ports/ioq3/build.sh" > /dev/null 2>&1 || { echo "ports/ioq3/build.sh failed"; exit 1; }
B="$B" bash "$REPO/ports/ioq3-gl2/build.sh" > /dev/null 2>&1 || { echo "ports/ioq3-gl2/build.sh failed"; exit 1; }
mkdir -p "$PKG/libs" "$PKG/bin" "$PKG/home/baseq3"
cp "$BS/ogles2.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc"
cp "$B/ioq3gl2/quake3-gl2" "$B"/ioq3gl2/*.dll "$BS/cmd/OGLES2Log" "$PKG/bin/"
cp "$B"/ioq3gl2/*.dll "$PKG/home/baseq3/"
