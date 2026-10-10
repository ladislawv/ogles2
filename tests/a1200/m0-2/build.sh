#!/bin/bash
# Fills the ogles2-m0-2 package directory ($1). Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}

make -C "$REPO" -j8 B="$B" > /dev/null
make -C "$REPO" B="$B" vbcc > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin"
cp "$B/ogles2.library" "$PKG/libs/"
cp "$B/tests/smoke" "$B/tests/abi_layout" "$B/tests/abi_probe" "$B/cmd/OGLES2Log" "$PKG/bin/"
cp "$B/vbcc/tests/smoke" "$PKG/bin/smoke_vbcc"
cp "$B/vbcc/tests/abi_probe" "$PKG/bin/abi_probe_vbcc"
