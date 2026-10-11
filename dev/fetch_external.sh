#!/bin/bash
# Fetches the reference sources of docs/DEVELOPMENT_PLAN.md section 3 into
# external/ (or $1), at the commits the plan was written against. Nothing in
# the build needs them: they are what the plan's comparisons, "Reuse" and
# "Template" entries refer to (PiStorm3D's driver, the MiniGL library glue,
# Mesa's V3D compiler and driver, the OS4 SDL and gl4es glue, glSnoop,
# Khronos registries, Emu68).
#   bash dev/fetch_external.sh [DIR]
set -eu
DIR=${1:-$(cd "$(dirname "$0")/.." && pwd)/external}
mkdir -p "$DIR"

# fetch NAME URL COMMIT [sparse paths...]
fetch() {
    local name=$1 url=$2 commit=$3
    shift 3
    local d=$DIR/$name
    if [ -d "$d/.git" ] && [ "$(git -C "$d" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
        echo "$name: already at ${commit:0:7}"
        return
    fi
    rm -rf "$d"
    mkdir -p "$d"
    git -C "$d" init -q
    git -C "$d" remote add origin "$url"
    if [ $# -gt 0 ]; then
        git -C "$d" sparse-checkout init --no-cone
        printf '%s\n' "$@" | git -C "$d" sparse-checkout set --no-cone --stdin
    fi
    # By commit when the server allows it, else the history without blobs.
    git -C "$d" fetch -q --depth 1 origin "$commit" 2>/dev/null ||
        git -C "$d" fetch -q --filter=blob:none origin
    git -C "$d" -c advice.detachedHead=false checkout -q "$commit"
    echo "$name: ${commit:0:7} from $url"
}

MINIGL=https://github.com/SteffenHaeuser/MiniGL_Library_68k.git
fetch pistorm3d-driver   $MINIGL 01f5a3e45fc761ca1534d8b07ba3bf638355be79   # branch PiStorm3D
fetch minigl-classic     $MINIGL d950a3a1ed640785a735f7d594298eaf3233b794   # branch MiniGL_Classic
fetch minigl-library-v3d $MINIGL 7a07368c57381b2ac81ceeb6bdf1245d7d4ba07b
fetch MiniGL_Library_68k $MINIGL b899c4779354d180ae17a4206077fe32287cd850
fetch Warp3DPiStorm4 https://github.com/SteffenHaeuser/Warp3DPiStorm4.git 541fc31141e8ee4005b58d5ed44d28a28db96917
fetch reference/mesa https://gitlab.freedesktop.org/mesa/mesa.git 74b69717c52b73494200e11e2dd97aa346d552dd \
    /src/broadcom/ /src/gallium/drivers/v3d/ /include/drm-uapi/v3d_drm.h /docs/drivers/v3d.rst /docs/license.rst
fetch reference/sdl-os4 https://github.com/AmigaPorts/SDL.git fca1fa7792c9441cce3ee43dddd4ef038600d9ac \
    /src/video/amigaos4/ /LICENSE.txt /docs/README-amigaos4.md
fetch reference/gl4es https://github.com/ptitSeb/gl4es.git ec16bedd8819c475326f4f1a3063772c6d986e06 \
    /src/agl/ /LICENSE /README.md /src/gl/shaderconv.c /src/gl/fpe_shader.c
fetch reference/glsnoop https://github.com/capehill/glsnoop.git d0173ba7e00c839090ccf83e178087dbb429faaf
fetch reference/opengl-registry https://github.com/KhronosGroup/OpenGL-Registry.git \
    6af574a14089ccfee87efe230ebcdd8742859813 /api/GLES2/ /xml/gl.xml /README.adoc
fetch reference/egl-registry https://github.com/KhronosGroup/EGL-Registry.git \
    db3425b8246136faccb5e2782b5694960bd6edf1 /api/KHR/
fetch reference/emu68 https://github.com/michalsc/Emu68.git 0ba3a899341dee958332a4182911861420b03bff
