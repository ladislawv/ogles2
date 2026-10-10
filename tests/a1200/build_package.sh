#!/bin/bash
# Assembles an A1200 test package from tests/a1200/<name>/ into
# tests/a1200/out/ogles2-<name>/ and tests/a1200/out/ogles2-<name>.lha.
# Run inside WSL:  bash build_package.sh <name>
#
# Each package directory holds:
#   build.sh   fills the package directory given as $1 (programs, third-party files)
#   textfiles  scripts and texts to copy with Amiga line ends (LF), checked to be ASCII
set -eu
NAME=${1:?package name}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/$NAME
OUT=$HERE/out
PKG=$OUT/ogles2-$NAME
LHA=~/tools/bin/lha

[ -f "$SRC/build.sh" ] && [ -f "$SRC/textfiles" ] || { echo "$SRC needs build.sh and textfiles"; exit 1; }
rm -rf "$PKG" "$OUT/ogles2-$NAME.lha"
mkdir -p "$PKG"

bash "$SRC/build.sh" "$PKG"

for f in $(tr -d '\r' < "$SRC/textfiles"); do
    sed 's/\r$//' "$SRC/$f" > "$PKG/$f"
    if LC_ALL=C grep -nP '[^\x00-\x7F]' "$PKG/$f"; then echo "non-ASCII in $f"; exit 1; fi
done

# Archive: lh5 compression, which every Amiga LhA/LZX/UnArc version reads.
(cd "$OUT" && $LHA -ao5q "ogles2-$NAME.lha" "ogles2-$NAME")
ls -l "$OUT"
(cd "$OUT" && $LHA -l "ogles2-$NAME.lha" | head -40)
sha256sum "$OUT/ogles2-$NAME.lha"
