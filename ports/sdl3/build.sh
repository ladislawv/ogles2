#!/bin/bash
# Builds SDL3 for AmigaOS 3 (68k) with OpenGL ES 2.0 on ogles2.library:
# $B/sdl3/libSDL3.a and libSDL3_test.a (clib2), and test programs in
# $B/sdl3/tests. Run in WSL after `make` (it needs libogles2.a and the
# generated headers):
#   bash ports/sdl3/build.sh
# The sources are not in this repository: Steffen Haeuser's SDL3_68k (the
# SDL3 of Aminet's dev/lib/SDL_68k, with minigl.library GL) is cloned into
# $SDL3_68K_SRC at the commit below and patched (patches/, src/). The MiniGL
# backend needs PiStorm3D's SDK ($PISTORM3D_SDK). ports/sdl3/README.md.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}
TC=${TC:-$HOME/amiga-gcc-install}
SDL3_68K_SRC=${SDL3_68K_SRC:-$HOME/sdl3-msn}
SDL3_68K_COMMIT=c82dea042718200eea401e7ed62d88b997bc9fd9 # 2026-10-06, SDL 3.4.14 (Aminet SDL_68k 2026-10-07)
PISTORM3D_SDK=${PISTORM3D_SDK:-/mnt/c/- CPP/pistorm3d/SDK}
O=$B/sdl3
export PATH="$TC/bin:$PATH" # the Makefile calls m68k-amigaos-gcc

[ -f "$B/libogles2.a" ] && [ -f "$B/gen/include/proto/ogles2.h" ] || { echo "run make first (B=$B)"; exit 1; }
[ -f "$PISTORM3D_SDK/lib/libminigl.a" ] || { echo "no PiStorm3D SDK at $PISTORM3D_SDK"; exit 1; }
if [ ! -d "$SDL3_68K_SRC/.git" ]; then
    git clone -q https://github.com/SteffenHaeuser/SDL3_68k.git "$SDL3_68K_SRC"
fi
# SDL3_68k clean (edits and files left in it are dropped), then ours.
git -C "$SDL3_68K_SRC" -c advice.detachedHead=false checkout -q "$SDL3_68K_COMMIT"
git -C "$SDL3_68K_SRC" checkout -q -- .
git -C "$SDL3_68K_SRC" clean -qfdx
for p in "$HERE"/patches/*.patch; do
    git -C "$SDL3_68K_SRC" apply "$p" || { echo "sdl3: $p does not apply"; exit 1; }
done
L=$SDL3_68K_SRC/SDL3
cp "$HERE"/src/video/amigaos3/SDL_os3opengles2.[ch] "$L/src/video/amigaos3/"

# Headers on paths without spaces (the Makefile splits its flags on them):
# ogles2's, and PiStorm3D's MiniGL SDK.
rm -rf "$O"
mkdir -p "$O/inc" "$O/lib" "$O/tests"
cp -r "$REPO"/include/. "$B"/gen/include/. "$O/inc/"
cp -r "$PISTORM3D_SDK" "$O/pistorm3d-sdk" # whole: its headers include ../../../backend/...
MGLINC=$O/pistorm3d-sdk/minigl-shared-library/include
cp "$PISTORM3D_SDK/lib/libminigl.a" "$O/lib/"

# clib2, the Makefile's own flags (68040, FPU; -fshort-wchar from the patch),
# plus ogles2's optimiser passes (-fbbb without "e", as the library's own
# Makefile).
FBBB=-fbbb=abcfhilmnprsz0
make -C "$L" -f Makefile.amigaos3 -j8 clib2 OGLES2_CFLAGS="-I$O/inc -I$MGLINC $FBBB" >"$O/make.log" 2>&1 ||
    { grep -E "error|Error" "$O/make.log" | head -30; echo "sdl3: make failed ($O/make.log)"; exit 1; }
cp "$L/build/clib2/libSDL3.a" "$L/build/clib2/libSDL3_test.a" "$O/"
echo "$O/libSDL3.a: $(stat -c %s "$O/libSDL3.a") bytes"

# Test programs: SDL3's own (test/), linked with libogles2.a
# (aglGetProcAddress) and MiniGL's import library. -lm881: clib2's maths
# (-lm would be libnix's).
CFLAGS=(-std=gnu99 -O2 -m68040 -mcrt=clib2 -mhard-float -fshort-wchar $FBBB -w -D__AMIGAOS3__ -DNOIXEMUL
        "-I$L/include" "-I$L/include/build_config" "-I$O/inc" "-I$MGLINC")
# tests/run_for.c starts each (--seconds N quits a test after N seconds of
# frames) and wraps the two present calls (its frame-time check): a program
# with SDL3's main callbacks through SDL_RunApp, wrapped; one with its own
# main as test_main, from run_for's main().
LIBS=(-Wl,--wrap=SDL_RenderPresent -Wl,--wrap=SDL_GL_SwapWindow
      -L"$O" -lSDL3_test -lSDL3 -L"$B" -logles2 -L"$O/lib" -lminigl -ldebug -lamiga -lm881 -latomic -lstubs)
m68k-amigaos-gcc "${CFLAGS[@]}" -c "$L/test/testutils.c" -o "$O/testutils.o"
m68k-amigaos-gcc "${CFLAGS[@]}" -c "$HERE/tests/run_for.c" -o "$O/run_for_cb.o"
m68k-amigaos-gcc "${CFLAGS[@]}" -DRUN_FOR_MAIN -c "$HERE/tests/run_for.c" -o "$O/run_for_main.o"
for t in testgles2 testsprite testrendercopyex testdraw; do
    if grep -q "define SDL_MAIN_USE_CALLBACKS" "$L/test/$t.c"; then
        m68k-amigaos-gcc "${CFLAGS[@]}" "$L/test/$t.c" "$O/testutils.o" "$O/run_for_cb.o" \
            -Wl,--wrap=SDL_RunApp "${LIBS[@]}" -o "$O/tests/$t"
    else
        # HAVE_OPENGLES2: testgles2 knows only some platforms have it
        m68k-amigaos-gcc "${CFLAGS[@]}" -DHAVE_OPENGLES2 -Dmain=test_main "$L/test/$t.c" "$O/testutils.o" \
            "$O/run_for_main.o" "${LIBS[@]}" -o "$O/tests/$t"
    fi
done
cp "$L/test/icon.png" "$L/test/sample.png" "$O/tests/"
ls -l "$O/tests"
