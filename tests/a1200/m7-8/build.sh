#!/bin/bash
# Fills the ogles2-m7-8 package directory ($1): a window that is all of a
# private screen (SDL's fullscreen) rendered straight into its screen, no
# copy (display.c, window_is_screen): m1_clear's SCREENWINDOW case beside
# the application window and fullscreen ones, then SDL2's testgles2 and
# testsprite2 and SDL3's testgles2 full screen, and testgles2 full screen
# with the copy kept (ENV:OGLES2/WindowCopy 1) for the comparison. SDL's
# programs write shaders at run time: the package brings the on-target
# compiler (libs/v3dglslc) and a cache directory. The sync-logging library.
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
B="$B" bash "$REPO/ports/sdl2/build.sh" > "$PKG.sdl2-build.log" 2>&1 || { tail -20 "$PKG.sdl2-build.log"; exit 1; }
B="$B" bash "$REPO/ports/sdl3/build.sh" > "$PKG.sdl3-build.log" 2>&1 || { tail -20 "$PKG.sdl3-build.log"; exit 1; }
rm -f "$PKG.sdl2-build.log" "$PKG.sdl3-build.log"
mkdir -p "$PKG/libs" "$PKG/bin" "$PKG/bin3"
cp "$BS/ogles2.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc"
cp "$BS/tests/m1_clear" "$BS/cmd/OGLES2Log" "$PKG/bin/"
cp "$B"/sdl2/tests/* "$PKG/bin/"
cp "$B"/sdl3/tests/* "$PKG/bin3/"
