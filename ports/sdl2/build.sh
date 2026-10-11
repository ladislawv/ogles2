#!/bin/bash
# Builds SDL2 for AmigaOS 3 (68k) with OpenGL ES 2.0 on ogles2.library:
# $B/sdl2/libSDL2.a and libSDL2_test.a (clib2), and test programs in
# $B/sdl2/tests. Run in WSL after `make` (it needs libogles2.a and the
# generated headers):
#   bash ports/sdl2/build.sh
# The sources are not in this repository: Steffen Haeuser's SDL2_68k (the
# SDL2 of Aminet's dev/lib/SDL_68k, with minigl.library GL) is cloned into
# $SDL68K_SRC at the commit below and patched (patches/, src/); SDL 2.30.3's
# testgles2 and testrendercopyex come from upstream ($SDLUP_SRC). The
# MiniGL backend needs PiStorm3D's SDK ($PISTORM3D_SDK). ports/sdl2/README.md.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}
TC=${TC:-$HOME/amiga-gcc-install}
SDL68K_SRC=${SDL68K_SRC:-$HOME/sdl2-msn}
SDL68K_COMMIT=3632f48dc4d085355954322ae452ecc7b007ebc6 # 2026-10-06, SDL 2.33.0 (Aminet SDL_68k 2026-10-07)
SDLUP_SRC=${SDLUP_SRC:-$HOME/sdl2-upstream}
SDLUP_COMMIT=fb1497566c5a05e2babdcf45ef0ab5c7cca2c4ae  # release-2.30.3
PISTORM3D_SDK=${PISTORM3D_SDK:-/mnt/c/- CPP/pistorm3d/SDK}
O=$B/sdl2
export PATH="$TC/bin:$PATH" # the Makefiles call m68k-amigaos-gcc and vasmm68k_mot

[ -f "$B/libogles2.a" ] && [ -f "$B/gen/include/proto/ogles2.h" ] || { echo "run make first (B=$B)"; exit 1; }
[ -f "$PISTORM3D_SDK/lib/libminigl.a" ] || { echo "no PiStorm3D SDK at $PISTORM3D_SDK"; exit 1; }
if [ ! -d "$SDL68K_SRC/.git" ]; then
    git clone -q https://github.com/SteffenHaeuser/SDL2_68k.git "$SDL68K_SRC"
fi
if [ ! -d "$SDLUP_SRC/.git" ]; then
    git clone -q --depth 1 -b release-2.30.3 https://github.com/libsdl-org/SDL.git "$SDLUP_SRC"
fi
git -C "$SDLUP_SRC" -c advice.detachedHead=false checkout -q "$SDLUP_COMMIT"
# SDL2_68k clean (edits and files left in it are dropped), then ours.
git -C "$SDL68K_SRC" -c advice.detachedHead=false checkout -q "$SDL68K_COMMIT"
git -C "$SDL68K_SRC" checkout -q -- .
git -C "$SDL68K_SRC" clean -qfdx
for p in "$HERE"/patches/*.patch; do
    git -C "$SDL68K_SRC" apply "$p" || { echo "sdl2: $p does not apply"; exit 1; }
done
L=$SDL68K_SRC/libSDL2-amigaos3-main
cp "$HERE"/src/video/amigaos3/SDL_os3opengles2.[ch] "$L/src/video/amigaos3/"

# Headers on paths without spaces (the Makefiles split CFLAGS on them):
# ogles2's, and PiStorm3D's MiniGL SDK.
rm -rf "$O"
mkdir -p "$O/inc" "$O/lib" "$O/tests"
cp -r "$REPO"/include/. "$B"/gen/include/. "$O/inc/"
cp -r "$PISTORM3D_SDK" "$O/pistorm3d-sdk" # whole: its headers include ../../../backend/...
MGLINC=$O/pistorm3d-sdk/minigl-shared-library/include
cp "$PISTORM3D_SDK/lib/libminigl.a" "$O/lib/"

# clib2, the Makefile's own flags (68040, FPU), plus ogles2's optimiser passes
# (-fbbb without "e", as the library's own Makefile).
FBBB=-fbbb=abcfhilmnprsz0
make -C "$L" -f Makefile -j8 OGLES2_CFLAGS="-I$O/inc -I$MGLINC $FBBB" >"$O/make.log" 2>&1 ||
    { grep -E "error|Error" "$O/make.log" | head -30; echo "sdl2: make failed ($O/make.log)"; exit 1; }
cp "$L/build/clib2/libSDL2.a" "$L/build/clib2/libSDL2_test.a" "$O/"
echo "$O/libSDL2.a: $(stat -c %s "$O/libSDL2.a") bytes"

# Test programs: SDL2_68k's adapted upstream ones and upstream's GLES2 ones,
# with its Amiga preamble; linked with libogles2.a (aglGetProcAddress) and
# MiniGL's import library. -lm881: clib2's maths (-lm would be libnix's).
BASE=(-std=gnu99 -O2 -m68040 -mcrt=clib2 -mhard-float $FBBB -w -D__AMIGAOS3__ "-I$L/include" "-I$O/inc" "-I$MGLINC"
      "-I$L/examples/upstream")
CFLAGS=("${BASE[@]}" -include "$L/examples/upstream/amiga_test_preamble.h") # programs only (it defines __stack)
LIBS=(-Wl,--wrap=SDL_RenderPresent -Wl,--wrap=SDL_GL_SwapWindow # run_for.c's frame-time check
      -L"$O" -lSDL2_test -lSDL2 -L"$B" -logles2 -L"$O/lib" -lminigl -ldebug -lamiga -lm881 -lstubs)
m68k-amigaos-gcc "${BASE[@]}" -c "$L/examples/upstream/testutils.c" -o "$O/testutils.o"
# Each with tests/run_for.c's main: --seconds N quits it after N seconds of frames
# (the test's main is test_main).
m68k-amigaos-gcc "${BASE[@]}" -c "$HERE/tests/run_for.c" -o "$O/run_for.o"
for t in testgles2 testrendercopyex; do
    # HAVE_OPENGLES2: testgles2 knows only some platforms have it
    m68k-amigaos-gcc "${CFLAGS[@]}" -DHAVE_OPENGLES2 -Dmain=test_main "$SDLUP_SRC/test/$t.c" "$O/testutils.o" "$O/run_for.o"         "${LIBS[@]}" -o "$O/tests/$t"
done
for t in testsprite2 testdraw2; do
    m68k-amigaos-gcc "${CFLAGS[@]}" -Dmain=test_main "$L/examples/upstream/$t.c" "$O/testutils.o" "$O/run_for.o"         "${LIBS[@]}" -o "$O/tests/$t"
done
cp "$L/examples/upstream/icon.bmp" "$SDLUP_SRC/test/sample.bmp" "$O/tests/"
ls -l "$O/tests"
