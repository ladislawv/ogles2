#!/bin/bash
# Fills the ogles2-gears package directory ($1): es2gears from the corpus
# (tests/corpus), live: it runs until Esc in its window or Ctrl-C.
# Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}
make -C "$REPO" -j8 B="$B" > /dev/null
B="$B" bash "$REPO/tests/corpus/build_corpus.sh" > /dev/null 2>&1 || { echo "build_corpus.sh failed"; exit 1; }
mkdir -p "$PKG/libs" "$PKG/bin"
cp "$B/ogles2.library" "$PKG/libs/"
cp "$B/corpus/amiga/es2gears" "$B"/corpus/v3dp/*.v3dp "$PKG/bin/"
