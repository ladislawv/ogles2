#!/bin/bash
# Builds gl4es (desktop OpenGL on GLES2, https://github.com/ptitSeb/gl4es) for
# AmigaOS 3 on ogles2.library: $B/gl4es/libgl4es.a. Run in WSL after `make`
# (it needs the generated dispatch header):
#   bash ports/gl4es/build.sh
# The gl4es sources are not in this repository: they are cloned into
# $GL4ES_SRC (default ~/gl4es) at the commit below. ports/gl4es/README.md.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}
TC=${TC:-$HOME/amiga-gcc-install}
GL4ES_SRC=${GL4ES_SRC:-$HOME/gl4es}
GL4ES_COMMIT=ec16bedd8819c475326f4f1a3063772c6d986e06 # the plan's reference (dev/fetch_external.sh)
CC=$TC/bin/m68k-amigaos-gcc
AR=$TC/bin/m68k-amigaos-ar
O=$B/gl4es

if [ ! -d "$GL4ES_SRC/.git" ]; then
    git clone -q https://github.com/ptitSeb/gl4es.git "$GL4ES_SRC"
fi
git -C "$GL4ES_SRC" -c advice.detachedHead=false checkout -q "$GL4ES_COMMIT"
# ogles2's patches (patches/*.patch, README.md), applied in order to a clean
# tree: edits left in the checkout are discarded first. GL4ES_EXTRA_PATCHES
# (paths) adds more after them, for diagnosis.
git -C "$GL4ES_SRC" checkout -q -- .
for p in "$HERE"/patches/*.patch ${GL4ES_EXTRA_PATCHES:-}; do
    git -C "$GL4ES_SRC" apply "$p" || { echo "gl4es: $p does not apply"; exit 1; }
done
[ -f "$B/gen/include/libraries/ogles2_dispatch.h" ] || { echo "run make first (B=$B)"; exit 1; }

rm -rf "$O"
mkdir -p "$O/obj"
# The compiler's -include and -I take no quoting through $FLAGS: a copy of
# the forced header in the build directory (the repository's path has a space).
cp "$HERE/os3_build.h" "$O/os3_build.h"
python3 "$HERE/gen_glue.py" "$B/gen/include/libraries/ogles2_dispatch.h" "$O/ogles2_glue.c"

# gl4es with its AmigaOS 4 code paths (no X11, EGL or dlopen; its agl
# glue), without a constructor (agl_os3.c starts it). The CPU flags are the
# library's (Makefile: -fbbb without "e", plan 2.3).
CPU="-mcpu=68020 -m68881 -fbbb=abcfhilmnprsz0"
# GCC's builtins on: gl4es copies and compares small state structs on every
# draw, and a library call for each was a good part of its 22 us a draw
# (Quake III, M6). Not sin and cos, as v3dcore's flags (Makefile).
NOBUILTIN="-fno-builtin-sin -fno-builtin-cos -fno-builtin-sinf -fno-builtin-cosf"
FLAGS="-std=gnu11 -O2 $CPU -mcrt=clib2 -fno-strict-aliasing $NOBUILTIN -w
 -DAMIGAOS4 -DNOX11 -DNOEGL -DNO_GBM -DSTATICLIB -DNO_INIT_CONSTRUCTOR -DDEFAULT_ES=2
 -include $O/os3_build.h
 -I$GL4ES_SRC/include -I$GL4ES_SRC/src -I$GL4ES_SRC/src/util -I$GL4ES_SRC/src/gl -I$GL4ES_SRC/src/glx"
SRCS=$(cd "$GL4ES_SRC/src" && echo gl/*.c gl/wrap/*.c gl/math/*.c glx/hardext.c glx/gbm.c glx/glx.c agl/lookup.c)
for f in $SRCS; do
    $CC $FLAGS -c "$GL4ES_SRC/src/$f" -o "$O/obj/$(echo "$f" | tr / _).o" &
    while [ "$(jobs -r | wc -l)" -ge 8 ]; do wait -n; done
done
wait
# The glue and agl: our headers (GLES2 types, the dispatch table), not gl4es's.
GLUE=(-std=gnu11 -O2 $CPU -mcrt=clib2 -fno-strict-aliasing -Wall "-I$REPO/include" "-I$B/gen/include")
$CC "${GLUE[@]}" -c "$O/ogles2_glue.c" -o "$O/obj/ogles2_glue.o"
$CC "${GLUE[@]}" -c "$HERE/agl_os3.c" -o "$O/obj/agl_os3.o"
$CC "${GLUE[@]}" -fno-builtin -c "$HERE/os3_string.c" -o "$O/obj/os3_string.o"
rm -f "$O/libgl4es.a"
$AR rcs "$O/libgl4es.a" "$O"/obj/*.o
echo "$O/libgl4es.a: $(ls "$O"/obj/*.o | wc -l) objects, $(stat -c %s "$O/libgl4es.a") bytes"
