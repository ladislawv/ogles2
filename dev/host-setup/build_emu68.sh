#!/bin/bash
# Builds Emu68 for PiStorm (Pi 4) from source: the base for the shader-compiler worker
# (docs/DEVELOPMENT_PLAN.md §8.4). Run inside WSL as the normal user.
# Packages (once, as root):
#   apt-get install cmake device-tree-compiler gcc-14-aarch64-linux-gnu g++-14-aarch64-linux-gnu \
#     gcc-14-powerpc-linux-gnu g++-14-powerpc-linux-gnu qemu-user
set -u
TAG=${TAG:-v1.1.0-beta.1}
VARIANT=${VARIANT:-pistorm}
SRC=~/emu68/src-$TAG
B=~/emu68/build-$TAG-$VARIANT
mkdir -p ~/emu68
if [ ! -d "$SRC" ]; then
    git clone --recursive -b "$TAG" https://github.com/michalsc/Emu68.git "$SRC" >~/emu68/clone.log 2>&1 || {
        echo "clone failed"; exit 1; }
fi
git -C "$SRC" log -1 --format='Emu68 %h %cs %d'
rm -rf "$B" && mkdir -p "$B/install" && cd "$B"
cmake "$SRC" -DCMAKE_TOOLCHAIN_FILE="$SRC/toolchains/aarch64-linux-gnu.cmake" \
    -DTARGET=raspi64 -DVARIANT="$VARIANT" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$B/install" >"$B/configure.log" 2>&1 || { echo "configure failed"; tail -20 "$B/configure.log"; exit 1; }
make -j24 >"$B/build.log" 2>&1 && make install >>"$B/build.log" 2>&1
rc=$?
echo "Emu68 $TAG $VARIANT build rc=$rc"
[ $rc -ne 0 ] && grep -E "error|Error" "$B/build.log" | head -10
ls -l "$B/install"
