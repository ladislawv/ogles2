#!/bin/bash
# Installs the two host tools the build and the tests use, at the versions
# ogles2 was developed with:
#   ~/tools/bin/lha    LHa for UNIX (jca02266) 1.14i-ac: test packages (.lha, lh5),
#                      vbcc's archives (build_vbcc.sh)
#   ~/amitools-venv    amitools (vamos, an AmigaOS emulator for the host tests and
#                      the built-in shader generator)
# Override with TOOLS=... and VENV=...; needs gcc, autoconf, automake, python3-venv.
set -eu
TOOLS=${TOOLS:-$HOME/tools}
VENV=${VENV:-$HOME/amitools-venv}
SRC=${SRC:-$HOME/tools-src}
LHA_COMMIT=16619b066b189ef289bb8b07b37d1c38d550da99
AMITOOLS_COMMIT=3b57f2052ee76c28bbc5e4256227f62dca7b1c9f

mkdir -p "$SRC"
if [ ! -x "$TOOLS/bin/lha" ]; then
    [ -d "$SRC/lha" ] || git clone -q https://github.com/jca02266/lha.git "$SRC/lha"
    git -C "$SRC/lha" -c advice.detachedHead=false checkout -q "$LHA_COMMIT"
    (cd "$SRC/lha" && autoreconf -is >/dev/null 2>&1 && ./configure -q --prefix="$TOOLS" &&
        make -s -j"$(nproc)" && make -s install) >/dev/null 2>&1
fi
"$TOOLS/bin/lha" --version 2>&1 | head -1

if [ ! -x "$VENV/bin/vamos" ]; then
    python3 -m venv "$VENV"
    # vamos's dependencies are not declared at this commit: the versions used here.
    "$VENV/bin/pip" install -q "git+https://github.com/cnvogelg/amitools.git@$AMITOOLS_COMMIT" \
        machine68k==0.4.1 greenlet==3.5.6
fi
"$VENV/bin/pip" show amitools | sed -n 2p
