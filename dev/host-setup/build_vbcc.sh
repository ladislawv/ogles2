#!/bin/bash
# Installs vbcc 0.9h patch 2 (m68k-amigaos target), vasm 1.9 (Motorola
# syntax) and vlink 0.17 into ~/vbcc, for the vbcc client test (plan
# section 6.3; `make vbcc` in the repository). The system headers come from
# the GCC 6 toolchain's NDK 3.2 (~/amiga-gcc-install/m68k-amigaos/ndk-include),
# so build_toolchains.sh must have run first.
# vbcc is free for non-commercial use (see its documentation).
set -eu
P=${VBCC:-$HOME/vbcc}
D=$HOME/vbcc-dl
LHA=${LHA:-$HOME/tools/bin/lha}
rm -rf "$D" && mkdir -p "$D" && cd "$D"
curl -fsSLO http://phoenix.owl.de/tags/vbcc0_9hP2.tar.gz
curl -fsSLO http://phoenix.owl.de/tags/vasm1_9.tar.gz
curl -fsSLO http://phoenix.owl.de/tags/vlink0_17.tar.gz
curl -fsSLO http://phoenix.owl.de/vbcc/current/vbcc_target_m68k-amigaos.lha
curl -fsSLO http://phoenix.owl.de/vbcc/current/vbcc_unix_config.tar.gz
# The files installed on 2026-10-10. The target archive lives in "current"
# and may change; a mismatch there is worth a look, not necessarily an error.
sha256sum -c - <<'EOF'
1986e21096a7a08f18699921c719098152f0e981468fa8bf651fe44681526482  vasm1_9.tar.gz
1f07497c76125eee1f40414779fb9c9aa547cc2a413b73d5b5adb31f9146bcbb  vbcc0_9hP2.tar.gz
ec734d7115359cdb5d1c70349284ecbd5712ef47e8bc28d4150117c6f8c73289  vbcc_target_m68k-amigaos.lha
c643a095439aaf0cbcf7521920c0ff5d1cb07a4fc6fe8cc59e69ae1dbb329497  vbcc_unix_config.tar.gz
e9596a0160149a423ca1c31fcb830f446b8deaa33d72c379555fa7097012a851  vlink0_17.tar.gz
EOF
mkdir x && cd x
for f in ../*.tar.gz; do tar xzf "$f"; done
"$LHA" -xq ../vbcc_target_m68k-amigaos.lha

mkdir -p "$P/bin" "$P/targets" "$P/config"
(cd vasm && make -s CPU=m68k SYNTAX=mot > ../vasm.log 2>&1) && cp vasm/vasmm68k_mot "$P/bin/"
(cd vlink && make -s > ../vlink.log 2>&1) && cp vlink/vlink "$P/bin/"
# vbcc's dtgen asks about the host's types: "y" for a cross-compiler, then
# the detected default for every type (empty answers).
(cd vbcc && mkdir -p bin && yes "" | make TARGET=m68k > ../vbcc.log 2>&1) &&
    cp vbcc/bin/vbccm68k vbcc/bin/vc vbcc/bin/vprof "$P/bin/"
cp -r vbcc_target_m68k-amigaos/targets/m68k-amigaos "$P/targets/"
cp config/aos68k config/aos68km config/aos68kr "$P/config/"
echo "vbcc installed in $P:"
ls "$P/bin"
