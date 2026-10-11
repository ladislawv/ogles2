#!/bin/bash
# Builds the corpus (plan section 13, M2 exit) for the PC and the Amiga,
# renders the reference images on the PC (Mesa llvmpipe), records the
# programs' shader sources there and compiles them with v3dglslc.
# Run inside WSL:  bash tests/corpus/build_corpus.sh
#
# The programs' sources are not in this repository: es2gears and eglut.h
# come from Mesa demos (MIT), the samples from the OpenGL ES 2.0
# Programming Guide's repository (danginsburg/opengles-book-samples), both
# cloned into $CORPUS_SRC. They are compiled unmodified; only the window
# and loop code is ours (esutil_corpus.c, eglut_corpus.c). programs/ holds
# three of our own in the book's style, for framebuffer objects (M5), which
# neither source has.
#
# Output in $B/corpus:
#   amiga/<name>      the m68k programs        host/<name>  the PC ones
#   ref/<name>.ppm    reference images          data/        the samples' TGAs
#   shaders.txt       the shader records        v3dp/        the program binaries
#   steps.txt         name and capture frame of each program, for the A1200 package
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
# The compiler command lines below are split on spaces: reach the repository
# through a link without one ("- CPP").
ln -sfn "$(cd "$HERE/../.." && pwd)" "$HOME/.ogles2-repo"
REPO=$HOME/.ogles2-repo
HERE=$REPO/tests/corpus
B=${B:-$HOME/ogles2-build/m68k}
CORPUS_SRC=${CORPUS_SRC:-$HOME/corpus-src}
FRAMES=${FRAMES:-60}
V3DGLSLC=${V3DGLSLC:-$HOME/ogles2-build/v3dglslc/host/v3dglslc}
TC=${TC:-$HOME/amiga-gcc-install}
OUT=$B/corpus
DEMOS=$CORPUS_SRC/demos
BOOK=$CORPUS_SRC/opengles-book-samples/LinuxX11

[ -d "$DEMOS" ] || git clone -q --depth 1 https://gitlab.freedesktop.org/mesa/demos.git "$DEMOS"
[ -d "$BOOK" ] || git clone -q --depth 1 https://github.com/danginsburg/opengles-book-samples.git \
    "$CORPUS_SRC/opengles-book-samples"

# name  source  framework  [the frame to capture: default the last]
# Stencil_Test: frame 1. It sets glStencilMask(0) at the end of each frame, so later
# frames read stencil values kept across eglSwapBuffers, which EGL leaves undefined
# (ogles2 does not keep them; Mesa's framebuffer object does).
PROGRAMS="
es2gears        $DEMOS/src/egl/opengles2/es2gears.c                eglut
Hello_Triangle  $BOOK/Chapter_2/Hello_Triangle/Hello_Triangle.c   esutil
Simple_VertexShader $BOOK/Chapter_8/Simple_VertexShader/Simple_VertexShader.c esutil
Simple_Texture2D $BOOK/Chapter_9/Simple_Texture2D/Simple_Texture2D.c esutil
MipMap2D        $BOOK/Chapter_9/MipMap2D/MipMap2D.c               esutil
TextureWrap     $BOOK/Chapter_9/TextureWrap/TextureWrap.c         esutil
Simple_TextureCubemap $BOOK/Chapter_9/Simple_TextureCubemap/Simple_TextureCubemap.c esutil
MultiTexture    $BOOK/Chapter_10/MultiTexture/MultiTexture.c      esutil
Stencil_Test    $BOOK/Chapter_11/Stencil_Test/Stencil_Test.c      esutil  1
Multisample     $BOOK/Chapter_11/Multisample/Multisample.c        esutil
ParticleSystem  $BOOK/Chapter_13/ParticleSystem/ParticleSystem.c  esutil  40
RTT_Cube        $HERE/programs/RTT_Cube.c                         esutil
PostFX          $HERE/programs/PostFX.c                           esutil
Orientation     $HERE/programs/Orientation.c                      esutil
"

rm -rf "$OUT"
mkdir -p "$OUT/amiga" "$OUT/host" "$OUT/ref" "$OUT/data" "$OUT/src"
cp "$BOOK"/Chapter_10/MultiTexture/*.tga "$BOOK"/Chapter_13/ParticleSystem/*.tga "$OUT/data/"

CPU="-mcpu=68020 -m68881 -fbbb=abcfhilmnprsz0"
# -fno-builtin: GCC would merge sinf/cosf pairs into cexpf, which clib2 lacks.
# -lm881, not -lm: with -mcrt=clib2 -m68881, -lm finds libnix's libm (lib/libm020/libm881/libm.a),
# whose functions give wrong results in a clib2 program (sqrtf 0, cosf with the wrong sign;
# found on the A1200 in M2 run 5). clib2's own 68881 libm is libm881.a.
AMIGA_CC="$TC/bin/m68k-amigaos-gcc -std=gnu99 -O2 -w -fno-builtin -fno-strict-aliasing $CPU -mcrt=clib2 \
  -I$REPO/tests/corpus/amiga_include -I$REPO/include -I$B/gen/include -include $REPO/tests/corpus/amiga_hooks.h"
HOST_CC="gcc -std=gnu99 -O1 -w -include $REPO/tests/corpus/host_hooks.h"

echo "$PROGRAMS" | while read -r name src fw shot; do
    [ -n "$name" ] || continue
    shot=${shot:-$FRAMES}
    echo "$name $shot" >> "$OUT/steps.txt"
    if [ "$fw" = eglut ]; then
        files="$src $DEMOS/src/util/matrix.c $HERE/eglut_corpus.c"
        incs="-I$DEMOS/src/egl/eglut -I$DEMOS/src/util"
    else
        files="$src $BOOK/Common/esShader.c $BOOK/Common/esShapes.c $BOOK/Common/esTransform.c $HERE/esutil_corpus.c"
        incs="-I$BOOK/Common"
    fi
    $AMIGA_CC $incs -I$HERE $files $HERE/corpus.c $HERE/corpus_amiga.c -L$B -logles2 -lm881 -o "$OUT/amiga/$name"
    $HOST_CC $incs -I$HERE $files $HERE/corpus.c $HERE/corpus_host.c -lEGL -lGLESv2 -lm -o "$OUT/host/$name"
    (cd "$OUT/data" && CORPUS_FRAMES=$FRAMES CORPUS_SHOT=$shot CORPUS_OUT="$OUT/ref/$name.ppm" \
        CORPUS_SHADERS="$OUT/shaders.txt" "$OUT/host/$name" > "$OUT/ref/$name.txt" 2>&1) ||
        echo "$name: the PC run failed (see ref/$name.txt)"
    echo "$name: $(grep -c . "$OUT/ref/$name.txt") lines of output, $(ls "$OUT/ref/$name.ppm" 2>/dev/null || echo no image)"
done

python3 "$REPO/v3dp/split_shaders.py" "$OUT/shaders.txt" "$OUT/src"
mkdir -p "$OUT/v3dp"
(cd "$OUT/src" && for v in *.vert; do "$V3DGLSLC" -o ../v3dp "$v" "${v%.vert}.frag" > "${v%.vert}.log" 2>&1 ||
    echo "$v: v3dglslc failed (src/${v%.vert}.log)"; done)
python3 "$REPO/v3dp/v3dp_hashname.py" "$OUT"/v3dp/*.v3dp
ls "$OUT/v3dp"
