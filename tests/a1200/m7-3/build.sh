#!/bin/bash
# Fills the ogles2-m7-3 package directory ($1): EXT_clip_control (m7_clip, the
# Z planes probe first; docs/M7_EXTENSIONS_PLAN.md step 4) and the earlier
# drawing, depth and framebuffer object tests. The
# sync-logging library: every log line reaches the file at once, so a GPU
# hang loses nothing. Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k-sync}
V=${V:-$HOME/ogles2-build/v3dglslc}

make -C "$REPO" -j8 B="$B" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" B="$B" test-v3dp > /dev/null
mkdir -p "$PKG/libs" "$PKG/compiler" "$PKG/bin"
cp "$B/ogles2.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/compiler/v3dglslc" # in LIBS: for step 2 only (m7_clip's program)
cp "$B/tests/m7_clip" "$B/tests/m1_draw" "$B/tests/m4_depth" "$B/tests/m5_fbo" "$B/cmd/OGLES2Log" "$PKG/bin/"
cp "$B"/v3dp/*.v3dp "$B"/v3dp/*.v3dw "$PKG/bin/"
