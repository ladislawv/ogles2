#!/bin/bash
# Builds dEQP-GLES2 (the drawElements GLES2 conformance tests of Khronos'
# VK-GL-CTS) for AmigaOS 3 (68020+68881, libnix) on ogles2.library:
# $B/deqp/deqp-gles2, with its data in $B/deqp/gles2. Run in WSL after
# `make` (libogles2.a):
#   bash ports/deqp/build.sh
# VK-GL-CTS (pinned below) is C++17, so it is built with GCC 16.2 ($TC16),
# as glmark2 is (ports/glmark2). Only what deqp-gles2 needs is compiled, by
# this script rather than the CTS's CMake (which would configure Vulkan,
# glslang and amber too): the framework (delibs, qphelper, tcutil, glutil,
# the reference renderer, the random shader generator), modules/glshared and
# modules/gles2. The CTS's own zlib and libpng (external/fetch_sources.py's
# versions) are downloaded into $DEPS. patches/ are applied to a copy of the
# sources in $B/deqp/src; src/ adds the AmigaOS 3 platform (one ogles2
# context in a window), delibs' OS layer for one task and libnix's gaps.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ln -sfn "$(cd "$HERE/../.." && pwd)" "$HOME/.ogles2-repo" # a path without the space of "- CPP"
REPO=$HOME/.ogles2-repo
B=${B:-$HOME/ogles2-build/m68k}
TC16=${TC16:-$HOME/amiga-gcc162-install}
J=${J:-8}
CTS_URL=${CTS_URL:-https://github.com/KhronosGroup/VK-GL-CTS.git}
CTS_TAG=opengl-es-cts-3.2.14.1
CTS_COMMIT=067e8832315e79817ede1c4863804e440f5d1c80
DEPS=${DEPS:-$HOME/deqp-deps}
ZLIB_URL=https://github.com/madler/zlib/releases/download/v1.2.13/zlib-1.2.13.tar.gz
ZLIB_SHA=b3a24de97a8fdbc835b9833169501030b8977031bcb54b3b3ac13740f846ab30
PNG_URL=https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.50.tar.gz
PNG_SHA=71158e53cfdf2877bc99bcab33641d78df3f48e6e0daad030afe9cb8c031aa46

# The CTS's history is several GB: a shallow clone of the release tag.
if [ -z "${CTS_SRC:-}" ]; then
    CTS_SRC=$HOME/vk-gl-cts
    if [ ! -d "$CTS_SRC/.git" ]; then
        GIT_TERMINAL_PROMPT=0 git clone -q --depth 1 --branch "$CTS_TAG" "$CTS_URL" "$CTS_SRC" ||
            { echo "could not clone $CTS_URL"; rm -rf "$CTS_SRC"; exit 1; }
    fi
fi
[ "$(git -C "$CTS_SRC" rev-parse HEAD)" = "$CTS_COMMIT" ] ||
    { echo "$CTS_SRC is not at $CTS_TAG ($CTS_COMMIT)"; exit 1; }

# fetch NAME URL SHA256: the archive unpacked into $DEPS/NAME.
fetch() {
    [ -d "$DEPS/$1" ] && return 0
    mkdir -p "$DEPS/dl"
    local a=$DEPS/dl/$1.tar.gz
    [ -f "$a" ] || curl -sSL -o "$a" "$2" || { echo "could not download $2"; return 1; }
    echo "$3  $a" | sha256sum -c --quiet - || { echo "$a: wrong checksum"; rm -f "$a"; return 1; }
    mkdir -p "$DEPS/$1.tmp"
    tar -xzf "$a" -C "$DEPS/$1.tmp" --strip-components=1
    mv "$DEPS/$1.tmp" "$DEPS/$1"
}
fetch zlib "$ZLIB_URL" "$ZLIB_SHA"
fetch libpng "$PNG_URL" "$PNG_SHA"
cp "$DEPS/libpng/scripts/pnglibconf.h.prebuilt" "$DEPS/libpng/pnglibconf.h"

[ -f "$B/libogles2.a" ] || { echo "run make first (B=$B)"; exit 1; }
[ -x "$TC16/bin/m68k-amigaos-g++" ] || { echo "no GCC 16.2 in $TC16 (CONTRIBUTING.md)"; exit 1; }

OUT=$B/deqp
ROOT=$OUT/src
OBJ=$OUT/obj
mkdir -p "$ROOT"
for d in framework modules/glshared modules/gles2 data/gles2; do
    mkdir -p "$ROOT/$d"
    rsync -a --delete "$CTS_SRC/$d/" "$ROOT/$d/"
done
for d in zlib libpng; do
    mkdir -p "$ROOT/external/$d"
    rsync -a --delete "$DEPS/$d/" "$ROOT/external/$d/"
done
for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    (cd "$ROOT" && git apply "$p") || { echo "deqp: $p does not apply"; exit 1; }
done
mkdir -p "$ROOT/amiga"
cp -r "$HERE"/src/. "$ROOT/amiga/"
mkdir -p "$OBJ"
rm -f "$OBJ/FAILED"

F=$ROOT/framework
D=$F/delibs
INC="-I$D/debase -I$D/depool -I$D/dethread -I$D/destream -I$D/deutil -I$D/decpp -I$F/qphelper
-I$F/common -I$F/xexml -I$F/opengl -I$F/opengl/wrapper -I$F/opengl/simplereference
-I$F/referencerenderer -I$F/randomshaders -I$ROOT/modules/glshared -I$ROOT/modules/gles2
-I$ROOT/modules/gles2/accuracy -I$ROOT/modules/gles2/functional -I$ROOT/modules/gles2/performance
-I$ROOT/modules/gles2/stress -I$ROOT/external/libpng -I$ROOT/external/zlib -I$F/egl -I$F/egl/wrapper
-I$ROOT/amiga/include -I$REPO/include"
# DE_OS_UNIX: libnix is close enough for delibs' file and string code; its
# threads, clock, sockets, processes and dynamic libraries are left out and
# src/deqp-amiga-os.c stands in for what is used. int32_t is int, as dEQP
# takes it to be everywhere (GLint is int32_t; std::max(int, int32_t)): this
# toolchain's is long, from GCC's __INT32_TYPE__, which newlib's
# <machine/_default_types.h> follows (the unsigned types then follow too).
DEFS="-DDE_OS=DE_OS_UNIX -DDE_PTR_SIZE=4 -DNDEBUG
-U__INT32_TYPE__ -D__INT32_TYPE__=int -U__UINT32_TYPE__
-U__INT_LEAST32_TYPE__ -D__INT_LEAST32_TYPE__=int -U__UINT_LEAST32_TYPE__
-DDEQP_TARGET_NAME=\"AmigaOS3\" -DDEQP_SUPPORT_GLES2=1 -DQP_SUPPORT_PNG -DPNG_ARM_NEON_OPT=0"
FLAGS="-O2 -mcpu=68020 -m68881 -noixemul -fno-strict-aliasing"
CXXFLAGS="-std=gnu++17 $FLAGS $DEFS $INC"
CFLAGS="-std=gnu99 $FLAGS $DEFS $INC -DZ_HAVE_UNISTD_H"

# Sources, relative to $ROOT: a directory, then files in it (as the CTS's
# CMakeLists.txt files list them); a directory may take several lines.
expand() {
    local d files f
    while read -r d files; do
        for f in $files; do echo "$d/$f"; done
    done
}
C_SRC=$(expand <<'EOF'
framework/delibs/debase deDefs.c deFloat16.c deInt32.c deMath.c deMemory.c deRandom.c deString.c
framework/delibs/debase deSha1.c
framework/delibs/depool deMemPool.c dePoolArray.c dePoolHeap.c dePoolHash.c dePoolHashArray.c
framework/delibs/depool dePoolHashSet.c dePoolMultiSet.c dePoolSet.c dePoolStringBuilder.c
framework/delibs/dethread deAtomic.c deSingleton.c
framework/delibs/deutil deCommandLine.c deFile.c
framework/qphelper qpCrashHandler.c qpDebugOut.c qpInfo.c qpTestLog.c qpWatchDog.c qpXmlWriter.c
amiga deqp-amiga-os.c
external/zlib adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c infback.c
external/zlib inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c
external/libpng png.c pngset.c pngget.c pngrutil.c pngtrans.c pngwutil.c pngread.c pngrio.c pngwio.c
external/libpng pngwrite.c pngrtran.c pngwtran.c pngmem.c pngerror.c pngpread.c
EOF
)
CXX_SRC="$(expand <<'EOF'
framework/delibs/decpp deAppendList.cpp deArrayBuffer.cpp deArrayUtil.cpp deBlockBuffer.cpp
framework/delibs/decpp deCommandLine.cpp deDefs.cpp deDirectoryIterator.cpp deDynamicLibrary.cpp
framework/delibs/decpp deFilePath.cpp deMemPool.cpp deMeta.cpp deMutex.cpp dePoolArray.cpp
framework/delibs/decpp dePoolString.cpp deRandom.cpp deRingBuffer.cpp deSemaphore.cpp deSTLUtil.cpp
framework/delibs/decpp deSharedPtr.cpp deStringUtil.cpp deThread.cpp deThreadLocal.cpp
framework/delibs/decpp deThreadSafeRingBuffer.cpp deUniquePtr.cpp deSpinBarrier.cpp deSha1.cpp
framework/xexml xeDefs.cpp xeXMLParser.cpp
framework/common tcuApp.cpp tcuArray.cpp tcuBilinearImageCompare.cpp tcuCommandLine.cpp
framework/common tcuCompressedTexture.cpp tcuDefs.cpp tcuFloat.cpp tcuFloatFormat.cpp
framework/common tcuFuzzyImageCompare.cpp tcuImageCompare.cpp tcuImageIO.cpp tcuInterval.cpp
framework/common tcuMatrix.cpp tcuPlatform.cpp tcuRGBA.cpp tcuRandomValueIterator.cpp
framework/common tcuRenderTarget.cpp tcuResource.cpp tcuResultCollector.cpp tcuSurface.cpp
framework/common tcuSurfaceAccess.cpp tcuTestCase.cpp tcuTestContext.cpp tcuTestSessionExecutor.cpp
framework/common tcuTestLog.cpp tcuTestPackage.cpp tcuTexture.cpp tcuTextureUtil.cpp
framework/common tcuFunctionLibrary.cpp tcuThreadUtil.cpp tcuStringTemplate.cpp tcuTexLookupVerifier.cpp
framework/common tcuTexCompareVerifier.cpp tcuTexVerifierUtil.cpp tcuCPUWarmup.cpp
framework/common tcuFactoryRegistry.cpp tcuSeedBuilder.cpp tcuMaybe.cpp tcuEither.cpp
framework/common tcuTestHierarchyIterator.cpp tcuTestHierarchyUtil.cpp tcuAstcUtil.cpp
framework/common tcuRasterizationVerifier.cpp tcuWaiverUtil.cpp
framework/opengl gluContextInfo.cpp gluDefs.cpp gluDrawUtil.cpp gluFboRenderContext.cpp
framework/opengl gluPixelTransfer.cpp gluProgramInterfaceQuery.cpp gluRenderConfig.cpp
framework/opengl gluRenderContext.cpp gluShaderProgram.cpp gluShaderUtil.cpp gluStateReset.cpp
framework/opengl gluTexture.cpp gluTextureUtil.cpp gluTextureTestUtil.cpp gluVarType.cpp
framework/opengl gluVarTypeUtil.cpp gluStrUtil.cpp gluCallLogWrapper.cpp gluObjectWrapper.cpp
framework/opengl gluContextFactory.cpp gluDummyRenderContext.cpp gluPlatform.cpp gluShaderLibrary.cpp
framework/opengl/wrapper glwDefs.cpp glwFunctions.cpp glwInitFunctions.cpp glwWrapper.cpp
framework/opengl/simplereference sglrContext.cpp sglrContextUtil.cpp sglrContextWrapper.cpp
framework/opengl/simplereference sglrReferenceContext.cpp sglrReferenceUtils.cpp sglrShaderProgram.cpp
framework/opengl/simplereference sglrGLContext.cpp
framework/referencerenderer rrDefs.cpp rrFragmentOperations.cpp rrMultisamplePixelBufferAccess.cpp
framework/referencerenderer rrPrimitivePacket.cpp rrRasterizer.cpp rrRenderer.cpp rrShaders.cpp
framework/referencerenderer rrShadingContext.cpp rrVertexAttrib.cpp rrVertexPacket.cpp
framework/platform tcuMain.cpp
amiga tcuAmigaPlatform.cpp amiga-compat.cpp
EOF
)
$(cd "$ROOT" && ls framework/randomshaders/rsg*.cpp | grep -v rsgTest.cpp)
$(cd "$ROOT" && ls modules/glshared/*.cpp modules/gles2/*.cpp modules/gles2/accuracy/*.cpp \
    modules/gles2/functional/*.cpp modules/gles2/performance/*.cpp modules/gles2/stress/*.cpp)"

# Compiles what changed, in up to $J jobs; a failure leaves its log beside its
# object. A change to this script, a patch or a header in src/include
# recompiles everything (headers are not tracked otherwise).
NEWEST=$(ls -t "$HERE/build.sh" "$HERE"/patches/* "$HERE"/src/include/* | head -1)
cc_one() { # compiler flags source
    local o=$OBJ/$(echo "$3" | tr / _).o
    o=${o%.*}.o
    [ "$o" -nt "$ROOT/$3" ] && [ "$o" -nt "$NEWEST" ] && return 0
    $1 $2 -c "$ROOT/$3" -o "$o" 2> "$o.log" || { echo "$3" >> "$OBJ/FAILED"; rm -f "$o"; }
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
if [ -f "$OBJ/FAILED" ]; then
    echo "FAILED ($(wc -l < "$OBJ/FAILED")):"
    cat "$OBJ/FAILED"
    exit 1
fi

"$TC16/bin/m68k-amigaos-g++" -mcpu=68020 -m68881 -noixemul -o "$OUT/deqp-gles2" "$OBJ"/*.o \
    -L"$B" -logles2 -lm
rm -rf "$OUT/gles2"
mkdir -p "$OUT/gles2"
cp -r "$ROOT/data/gles2/data" "$ROOT/data/gles2/shaders" "$OUT/gles2/"
ls -l "$OUT/deqp-gles2"
