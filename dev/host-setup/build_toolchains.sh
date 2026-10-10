#!/bin/bash
# Builds the two m68k-amigaos toolchains used by the ogles2 plan.
#   ~/amiga-gcc-install     GCC 6.5.0b (bebbo amiga6, default branch) + clib2 - PiStorm3D's toolchain
#   ~/amiga-gcc162-install  GCC 16.2.0b (bebbo amiga16.2) + libnix          - C++17 for the Mesa compiler port
# Verified 2026-10-10 at amiga6 eb241170d and amiga16.2 134541b3686. Both branches move;
# re-run dev/spike-c/scan_asm.sh after updating either (callee-saved-register check).
# TARGETS=gcc16 builds the older amiga16.1 into ~/amiga-gcc16-install, for comparison only.
# Run inside WSL as the normal user. Logs go to ~/toolchain-build/*.log.
set -u
J=${J:-24}
LOG=~/toolchain-build
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$LOG"
# Not "make all": it adds libSDL12, which fails on amiga6 and nothing here needs.
# libdebug is left out too: adtools libdebug conflicts with NDK 3.2's debug_protos.h,
# and libnix already installs a libdebug.a (KPrintF, KPutStr, KPutChar).
REQUIRED="gcc binutils gdb gprof fd2sfd fd2pragma sfdc vasm libnix libpthread libgcc ndk ndk13 libnix4.library"

log() { echo "$*" | tee -a "$LOG/summary.log"; }

build() {
    local name=$1 prefix=$2 branch=$3 with_clib2=$4
    local src=~/toolchain-build/src-$name
    log "=== $name: $(date)"
    if [ ! -d "$src" ]; then
        git clone https://codeberg.org/bebbo/amiga-gcc "$src" >"$LOG/$name-clone.log" 2>&1 || {
            log "clone failed"; return 1; }
    fi
    cd "$src" || return 1
    make update >"$LOG/$name-update.log" 2>&1 || { log "update failed"; return 1; }
    if [ -n "$branch" ]; then
        make branch branch="$branch" mod=gcc >"$LOG/$name-branch.log" 2>&1 || {
            log "branch switch failed"; return 1; }
    fi
    # libpthread calls timersub(), which no header defines: GCC 6 leaves an unresolved
    # _timersub, GCC 14+ stops with an error, and amiga-gcc builds libgcc and libstdc++
    # only after libpthread. pthread-timersub.patch adds the BSD macro.
    if ! git -C projects/aros-stuff apply --reverse --check "$HERE/pthread-timersub.patch" 2>/dev/null; then
        git -C projects/aros-stuff apply "$HERE/pthread-timersub.patch" || { log "pthread patch failed"; return 1; }
    fi
    make $REQUIRED -j"$J" PREFIX="$prefix" >"$LOG/$name-build.log" 2>&1
    local rc=$?
    log "$name make required rc=$rc $(date)"

    if [ "$with_clib2" = yes ]; then
        # amiga-gcc fetches adtools/clib2 master (c026659, 2021), which does not build
        # against NDK 3.2 as shipped; clib2-ndk32.patch fixes the three causes.
        [ -d projects/clib2 ] || git clone -b master --depth 4 https://github.com/adtools/clib2 projects/clib2 \
            >"$LOG/$name-clib2-clone.log" 2>&1
        if ! git -C projects/clib2 apply --reverse --check "$HERE/clib2-ndk32.patch" 2>/dev/null; then
            git -C projects/clib2 apply "$HERE/clib2-ndk32.patch" || { log "clib2 patch failed"; return 1; }
        fi
        # clib2's own makefile runs serially.
        make clib2 PREFIX="$prefix" >"$LOG/$name-clib2.log" 2>&1
        local rc2=$?
        log "$name make clib2 rc=$rc2 $(date)"
        [ $rc -eq 0 ] && rc=$rc2
    fi

    "$prefix/bin/m68k-amigaos-gcc" --version 2>&1 | head -1 | tee -a "$LOG/summary.log"
    (cd "$src" && git log -1 --format='amiga-gcc %h %ad' --date=short) | tee -a "$LOG/summary.log"
    (cd "$src/projects/gcc" 2>/dev/null && git log -1 --format='gcc %h %ad %d' --date=short) | tee -a "$LOG/summary.log"
    [ "$with_clib2" = yes ] && (cd "$src/projects/clib2" && git log -1 --format='clib2 %h %ad (+ clib2-ndk32.patch)' --date=short) | tee -a "$LOG/summary.log"
    return $rc
}

# clib2 is GCC 6 only: under GCC 16 its inline asm (multi-line asm strings, register
# variables named in clobber lists) no longer compiles. GCC 16 code uses libnix (-mcrt=nix20).
#   gcc162: GCC 16 from the newer amiga16.2 branch. amiga16.1 (2026-08-16) miscompiles:
#           functions write callee-saved registers they never save (dev/spike-c/README.md).
for t in ${TARGETS:-gcc6 gcc162}; do
    case $t in
        gcc6)   build gcc6   ~/amiga-gcc-install    ""        yes ;;
        gcc16)  build gcc16  ~/amiga-gcc16-install  amiga16.1 no ;;
        gcc162) build gcc162 ~/amiga-gcc162-install amiga16.2 no ;;
        *) log "unknown target $t" ;;
    esac
done
log "ALL DONE $(date)"
