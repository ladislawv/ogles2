#!/bin/bash
# Fills the ogles2-m7-7 package directory ($1): SDL2 with the fixes of
# ports/sdl2/patches/0002 (GitHub issue 5): audio through AHI and Paula
# (audiotest: the audio thread keeps playing), fullscreen on PiStorm RTG (a
# 32-bit "SDL" screen while it runs, none left behind: screens), and SDL's
# GLES2 tests in a window as before. SDL's renderer and testgles2 write
# shaders at run time: the package brings the on-target compiler
# (libs/v3dglslc) and a cache directory. The sync-logging library. Called
# by ../build_package.sh.
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
cp "$B"/sdl2/tests/* "$BS/cmd/OGLES2Log" "$PKG/bin/"
