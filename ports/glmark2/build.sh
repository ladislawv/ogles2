#!/bin/bash
# Builds glmark2-es2 for AmigaOS 3 (68020+68881, libnix) on ogles2.library:
# $B/glmark2/glmark2-es2, with its data in $B/glmark2/data. Run in WSL after
# `make` (libogles2.a):
#   bash ports/glmark2/build.sh
# glmark2 (github.com/glmark2/glmark2, pinned below) is C++17 with
# std::filesystem, so it is built with GCC 16.2 ($TC16), not the library's
# GCC 6.5; both write Amiga hunk objects and libogles2.a is plain C, so it
# links as it is. The upstream tree is copied to $B/glmark2/src, patches/
# applied there (main.cpp: the AMIGA native state and the OGLES2 GL state)
# and src/ added: NativeStateAmiga makes the context and its window,
# GLStateOGLES2 loads the GL functions through aglGetProcAddress. Bundled
# libpng, libjpeg-turbo and zlib, as upstream's meson build without system
# libraries. Shaders are compiled on the Amiga when first used (OGLES2Cache).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ln -sfn "$(cd "$HERE/../.." && pwd)" "$HOME/.ogles2-repo" # a path without the space of "- CPP"
REPO=$HOME/.ogles2-repo
B=${B:-$HOME/ogles2-build/m68k}
TC16=${TC16:-$HOME/amiga-gcc162-install}
J=${J:-8}
GLMARK2_URL=${GLMARK2_URL:-https://github.com/glmark2/glmark2.git}
GLMARK2_COMMIT=22c527cb0556f3a1ac4445aaa52cc532760928d5 # 2025-09-23
. "$REPO/ports/ioq3/sources.sh" --functions-only
if [ -z "${GLMARK2_SRC:-}" ]; then
    GLMARK2_SRC=$HOME/glmark2
    clone_pinned "$GLMARK2_SRC" "$GLMARK2_URL" "$GLMARK2_COMMIT" || exit 1
fi
[ -f "$B/libogles2.a" ] || { echo "run make first (B=$B)"; exit 1; }
[ -x "$TC16/bin/m68k-amigaos-g++" ] || { echo "no GCC 16.2 in $TC16 (CONTRIBUTING.md)"; exit 1; }

OUT=$B/glmark2
ROOT=$OUT/src
OBJ=$OUT/obj
mkdir -p "$ROOT"
rsync -a --delete --exclude .git "$GLMARK2_SRC/" "$ROOT/"
for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    (cd "$ROOT" && git apply "$p") || { echo "glmark2: $p does not apply"; exit 1; }
done
cp "$HERE"/src/* "$ROOT/src/"
rm -rf "$OBJ"
mkdir -p "$OBJ"

S=$ROOT/src
FLAGS="-O2 -mcpu=68020 -m68881 -noixemul -fno-strict-aliasing"
CXXFLAGS="-std=gnu++17 $FLAGS -DGLMARK2_USE_AMIGA=1 -DGLMARK2_USE_OGLES2=1 -DGLMARK2_USE_GLESv2=1 \
-DUSE_EXCEPTIONS -DGLMARK_VERSION=\"2023.01-amiga\" -DGLMARK_DATA_PATH=\"PROGDIR:data\" \
-DGLMARK2_EXECUTABLE=\"glmark2-es2\" \
-I$S -I$S/glad/include -I$S/libmatrix -I$S/scene-ideas -I$S/scene-terrain -I$S/libpng -I$S/libjpeg-turbo -I$S/zlib -I$REPO/include"
CFLAGS="-std=gnu99 $FLAGS -DZ_HAVE_UNISTD_H -I$S/glad/include -I$S/libpng -I$S/libjpeg-turbo -I$S/zlib"

# glmark2's common sources (src/meson.build) without shared-library.cpp
# (dlopen, for the EGL state only), main.cpp and the Amiga states.
CXX_SRC="benchmark-collection.cpp benchmark.cpp canvas-generic.cpp gl-headers.cpp gl-visual-config.cpp
image-reader.cpp libmatrix/log.cc libmatrix/mat.cc libmatrix/program.cc libmatrix/shader-source.cc
libmatrix/util.cc main-loop.cpp mesh.cpp model.cpp options.cpp results-file.cpp scene-buffer.cpp
scene-build.cpp scene-bump.cpp scene-clear.cpp scene-conditionals.cpp scene.cpp scene-default-options.cpp
scene-desktop.cpp scene-effect-2d.cpp scene-function.cpp scene-grid.cpp scene-ideas/a.cc scene-ideas.cpp
scene-ideas/d.cc scene-ideas/e.cc scene-ideas/i.cc scene-ideas/lamp.cc scene-ideas/logo.cc scene-ideas/m.cc
scene-ideas/n.cc scene-ideas/o.cc scene-ideas/s.cc scene-ideas/splines.cc scene-ideas/table.cc
scene-ideas/t.cc scene-jellyfish.cpp scene-loop.cpp scene-pulsar.cpp scene-refract.cpp scene-shading.cpp
scene-shadow.cpp scene-terrain/base-renderer.cpp scene-terrain/blur-renderer.cpp
scene-terrain/copy-renderer.cpp scene-terrain.cpp scene-terrain/luminance-renderer.cpp
scene-terrain/normal-from-height-renderer.cpp scene-terrain/overlay-renderer.cpp
scene-terrain/renderer-chain.cpp scene-terrain/simplex-noise-renderer.cpp
scene-terrain/terrain-renderer.cpp scene-terrain/texture-renderer.cpp scene-texture.cpp
text-renderer.cpp texture.cpp main.cpp native-state-amiga.cpp gl-state-ogles2.cpp amiga-compat.cpp"
C_SRC="glad/src/gles2.c
zlib/adler32.c zlib/compress.c zlib/crc32.c zlib/deflate.c zlib/gzclose.c zlib/gzlib.c zlib/gzread.c
zlib/gzwrite.c zlib/infback.c zlib/inffast.c zlib/inflate.c zlib/inftrees.c zlib/trees.c zlib/uncompr.c
zlib/zutil.c
libpng/png.c libpng/pngerror.c libpng/pnggccrd.c libpng/pngget.c libpng/pngmem.c libpng/pngpread.c
libpng/pngread.c libpng/pngrio.c libpng/pngrtran.c libpng/pngrutil.c libpng/pngset.c libpng/pngtrans.c
libpng/pngvcrd.c libpng/pngwio.c libpng/pngwrite.c libpng/pngwtran.c libpng/pngwutil.c
libjpeg-turbo/jcapimin.c libjpeg-turbo/jcapistd.c libjpeg-turbo/jccoefct.c libjpeg-turbo/jccolor.c
libjpeg-turbo/jcdctmgr.c libjpeg-turbo/jchuff.c libjpeg-turbo/jcinit.c libjpeg-turbo/jcmainct.c
libjpeg-turbo/jcmarker.c libjpeg-turbo/jcmaster.c libjpeg-turbo/jcomapi.c libjpeg-turbo/jcparam.c
libjpeg-turbo/jcphuff.c libjpeg-turbo/jcprepct.c libjpeg-turbo/jcsample.c libjpeg-turbo/jdapimin.c
libjpeg-turbo/jdapistd.c libjpeg-turbo/jdatadst.c libjpeg-turbo/jdatasrc.c libjpeg-turbo/jdcoefct.c
libjpeg-turbo/jdcolor.c libjpeg-turbo/jddctmgr.c libjpeg-turbo/jdhuff.c libjpeg-turbo/jdinput.c
libjpeg-turbo/jdmainct.c libjpeg-turbo/jdmarker.c libjpeg-turbo/jdmaster.c libjpeg-turbo/jdmerge.c
libjpeg-turbo/jdphuff.c libjpeg-turbo/jdpostct.c libjpeg-turbo/jdsample.c libjpeg-turbo/jerror.c
libjpeg-turbo/jfdctflt.c libjpeg-turbo/jfdctfst.c libjpeg-turbo/jfdctint.c libjpeg-turbo/jidctflt.c
libjpeg-turbo/jidctfst.c libjpeg-turbo/jidctint.c libjpeg-turbo/jidctred.c libjpeg-turbo/jmemmgr.c
libjpeg-turbo/jmemnobs.c libjpeg-turbo/jquant1.c libjpeg-turbo/jquant2.c libjpeg-turbo/jsimd_none.c
libjpeg-turbo/jutils.c"

# Compiles in up to $J jobs; a failure leaves its log in $OBJ/FAILED.
cc_one() { # compiler flags source
    local o=$OBJ/$(echo "$3" | tr / _).o
    o=${o%.*}.o
    $1 $2 -c "$S/$3" -o "$o" 2> "$o.log" || { echo "$3" >> "$OBJ/FAILED"; cat "$o.log"; }
}
for f in $CXX_SRC; do
    while [ "$(jobs -rp | wc -l)" -ge "$J" ]; do wait -n || true; done
    cc_one "$TC16/bin/m68k-amigaos-g++" "$CXXFLAGS" "$f" &
done
for f in $C_SRC; do
    while [ "$(jobs -rp | wc -l)" -ge "$J" ]; do wait -n || true; done
    cc_one "$TC16/bin/m68k-amigaos-gcc" "$CFLAGS" "$f" &
done
wait
[ ! -f "$OBJ/FAILED" ] || { echo "FAILED:"; cat "$OBJ/FAILED"; exit 1; }

"$TC16/bin/m68k-amigaos-g++" -mcpu=68020 -m68881 -noixemul -o "$OUT/glmark2-es2" "$OBJ"/*.o \
    -L"$B" -logles2 -lm
rm -rf "$OUT/data"
cp -r "$ROOT/data" "$OUT/data"
rm -f "$OUT/data/meson.build"
ls -l "$OUT/glmark2-es2"
