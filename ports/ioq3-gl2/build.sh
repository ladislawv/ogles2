#!/bin/bash
# Builds Quake III Arena for AmigaOS 3 (68k) with ioquake3's GLES2 renderer
# (renderergl2) on ogles2.library directly, without gl4es: $B/ioq3gl2/quake3-gl2
# and the game modules. Run in WSL after `make`, `make gl4es` and
# ports/ioq3/build.sh (its engine objects and game .dlls are reused: only the
# renderer differs):
#   bash ports/ioq3-gl2/build.sh
# The engine is Cowcat's Amiga ioquake3 ($IOQ3_SRC, as ports/ioq3); the
# renderer is upstream's renderergl2 + renderercommon ($IOQ3_UP_SRC, copied
# unchanged into $B/ioq3gl2/src beside the engine's sources, with upstream's
# header-only qcommon/json.h), compiled against the engine's qcommon headers
# with include/ioq3_gl2_compat.h forced in and include/ (SDL_opengl.h, SDL.h)
# first. amiga_glimp_gl2.c replaces sdl_glimp.c; ioq3_gl2_ref.c adapts the
# engine's refimport_t to upstream's. docs/IOQ3_GLES2_PORT_PLAN.md.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ln -sfn "$(cd "$HERE/../.." && pwd)" "$HOME/.ogles2-repo" # a path without the space of "- CPP"
REPO=$HOME/.ogles2-repo
B=${B:-$HOME/ogles2-build/m68k}
TC=${TC:-$HOME/amiga-gcc-install}
IOQ3_SRC=${IOQ3_SRC:-"/mnt/c/- CPP/Quake3/MiniGL_Library_Quake3/ioq3"}
IOQ3_UP_SRC=${IOQ3_UP_SRC:-"/mnt/c/- CPP/Quake3/ioq3-upstream"}
GL4ES_SRC=${GL4ES_SRC:-$HOME/gl4es} # its desktop GL headers only
GCC=$TC/bin/m68k-amigaos-gcc
LIBM020=$TC/m68k-amigaos/clib2/lib/libm020
ENGINE=$B/ioq3/obj
ROOT=$B/ioq3gl2/src
OUT=$B/ioq3gl2/obj
BUILDOUT=$B/ioq3gl2
[ -f "$ENGINE/cl_main.o" ] && [ -f "$B/ioq3/cgame68k.dll" ] || { echo "run ports/ioq3/build.sh first (B=$B)"; exit 1; }
[ -f "$B/libogles2.a" ] || { echo "run make first (B=$B)"; exit 1; }

mkdir -p "$ROOT"
rsync -a --delete "$IOQ3_SRC/code/" "$ROOT/code/"
rsync -a --delete "$IOQ3_UP_SRC/code/renderergl2/" "$ROOT/code/renderergl2/"
rsync -a --delete "$IOQ3_UP_SRC/code/renderercommon/" "$ROOT/code/renderercommon/"
cp "$IOQ3_UP_SRC/code/qcommon/json.h" "$ROOT/code/qcommon/json.h"
for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    (cd "$ROOT" && git apply "$p") || { echo "ioq3-gl2: $p does not apply"; exit 1; }
done
mkdir -p "$ROOT/code/amiga-gl2"
cp "$HERE/amiga_glimp_gl2.c" "$HERE/ioq3_gl2_ref.c" "$ROOT/code/amiga-gl2/"
rm -rf "$OUT"
mkdir -p "$OUT/glsl"

# The engine's flags (ports/ioq3), and for the renderer the compatibility
# header, the GL headers and the engine's jpeg-8c.
CFLAGS="-std=c99 -O2 -finline-functions -mcpu=68020 -m68881 -mcrt=clib2 \
-fno-strict-aliasing -fno-builtin-cos -fno-builtin-sin \
-DAMIGAOS -DNDEBUG -DBOTLIB -DUSE_LOCAL_HEADERS -DNO_VM_COMPILED"
RFLAGS="-include $REPO/ports/ioq3-gl2/include/ioq3_gl2_compat.h -I$REPO/ports/ioq3-gl2/include \
-I$GL4ES_SRC/include -I$ROOT/code/jpeg-8c -DUSE_INTERNAL_JPEG -DGetRefAPI=GL2_GetRefAPI"

cc() { # source object [extra flags...]
    local src=$1 obj=$2
    shift 2
    $GCC $CFLAGS "$@" -c "$src" -o "$obj" || { echo "FAILED: $src"; exit 1; }
}

# The shaders as C strings (upstream's cmake/utils/stringify_shader.cmake).
GLSL=""
for f in "$ROOT"/code/renderergl2/glsl/*.glsl; do
    n=$(basename "$f" .glsl)
    python3 -I - "$f" "$OUT/glsl/$n.c" "$n" <<'PY'
import sys
src, out, name = sys.argv[1:4]
text = open(src, encoding="latin-1").read().replace("\r", "")
body = text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", '\\n"\n"')
open(out, "w", encoding="latin-1").write('const char *fallbackShader_%s =\n"%s";\n' % (name, body))
PY
    cc "$OUT/glsl/$n.c" "$OUT/glsl/$n.o"
    GLSL="$GLSL $OUT/glsl/$n.o"
done

RENDERER=""
cd "$ROOT/code/renderergl2"
for f in tr_animation tr_backend tr_bsp tr_cmds tr_curve tr_dsa tr_extensions tr_extramath tr_fbo \
         tr_flares tr_glsl tr_image tr_image_dds tr_init tr_light tr_main tr_marks tr_mesh tr_model \
         tr_model_iqm tr_postprocess tr_scene tr_shade tr_shade_calc tr_shader tr_shadows tr_sky \
         tr_surface tr_vbo tr_world; do
    cc $f.c "$OUT/$f.o" $RFLAGS
    RENDERER="$RENDERER $OUT/$f.o"
done
# renderercommon without tr_subs.c (a separately loaded renderer's Com_Printf)
# and puff.c (the engine has it).
cd "$ROOT/code/renderercommon"
for f in tr_font tr_image_bmp tr_image_jpg tr_image_pcx tr_image_png tr_image_pvr tr_image_tga tr_noise; do
    cc $f.c "$OUT/rc_$f.o" $RFLAGS
    RENDERER="$RENDERER $OUT/rc_$f.o"
done
cd "$ROOT/code/amiga-gl2"
cc amiga_glimp_gl2.c "$OUT/amiga_glimp_gl2.o" $RFLAGS -I"$REPO/include"
# The GL functions: gl4es's generated glue over ogles2.library's dispatch
# table (QUAKE3_GL_PROFILE times every call).
python3 "$REPO/ports/gl4es/gen_glue.py" "$B/gen/include/libraries/ogles2_dispatch.h" "$OUT/ogles2_glue.c" \
    QUAKE3_GL_PROFILE > /dev/null
cc "$OUT/ogles2_glue.c" "$OUT/ogles2_glue.o" -I"$REPO/include" -I"$B/gen/include"
# Not -DGetRefAPI: it defines the engine's GetRefAPI and calls GL2_GetRefAPI.
cc ioq3_gl2_ref.c "$OUT/ioq3_gl2_ref.o" $(echo "$RFLAGS" | sed 's/-DGetRefAPI=GL2_GetRefAPI//')

# The engine: ports/ioq3's objects without its renderer and glimp.
MAIN=$(ls "$ENGINE"/*.o | grep -v -E '/(tr_[a-z_]*|amiga_glimp)\.o$' | tr '\n' ' ')
$GCC -mcpu=68020 -m68881 -mcrt=clib2 -o "$BUILDOUT/quake3-gl2" $MAIN $RENDERER $GLSL \
    "$OUT/amiga_glimp_gl2.o" "$OUT/ioq3_gl2_ref.o" "$OUT/ogles2_glue.o" \
    -L"$B" -logles2 -L"$LIBM020" -lm881 -lm -lc -lamiga 2>&1 | grep -v lang_reloc || true
[ -f "$BUILDOUT/quake3-gl2" ] || { echo "link failed"; exit 1; }
cp "$B"/ioq3/*.dll "$BUILDOUT/"
ls -l "$BUILDOUT"/quake3-gl2 "$BUILDOUT"/*.dll
