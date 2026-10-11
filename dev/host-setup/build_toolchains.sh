#!/bin/bash
# Builds the two m68k-amigaos toolchains used by the ogles2 plan.
#   ~/amiga-gcc-install     GCC 6.5.0b (bebbo amiga6, default branch) + clib2 - PiStorm3D's toolchain
#   ~/amiga-gcc162-install  GCC 16.2.0b (bebbo amiga16.2) + libnix          - C++17 for the Mesa compiler port
# Verified 2026-10-10 at amiga6 eb241170d and amiga16.2 134541b3686. Both branches move:
# by default (PINNED=1) every project is checked out at the commit in toolchain-pins.txt,
# so a new machine gets the tested compilers. PINNED=0 builds the branch heads instead;
# re-run tools/v3dglslc/scan_asm.sh after that (callee-saved-register check) and record
# the new commits in toolchain-pins.txt.
# TARGETS=gcc16 builds the older amiga16.1 into ~/amiga-gcc16-install, for comparison only.
# Run inside WSL (or any Linux) as the normal user. Logs go to ~/toolchain-build/*.log.
set -u
J=${J:-$(nproc)}
PINNED=${PINNED:-1}
LOG=~/toolchain-build
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$LOG"
# Not "make all": it adds libSDL12, which fails on amiga6 and nothing here needs.
# libdebug is left out too: adtools libdebug conflicts with NDK 3.2's debug_protos.h,
# and libnix already installs a libdebug.a (KPrintF, KPutStr, KPutChar).
REQUIRED="gcc binutils gdb gprof fd2sfd fd2pragma sfdc vasm libnix libpthread libgcc ndk ndk13 libnix4.library"

log() { echo "$*" | tee -a "$LOG/summary.log"; }

# pin DIR COMMIT: check DIR (a git work tree) out at COMMIT, fetching it if needed.
pin() {
    local dir=$1 commit=$2
    git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null ||
        git -C "$dir" fetch -q origin "$commit" 2>/dev/null ||
        git -C "$dir" fetch -q --unshallow 2>/dev/null || git -C "$dir" fetch -q origin
    git -C "$dir" -c advice.detachedHead=false checkout -q "$commit"
}

# pins TARGET STAGE: apply toolchain-pins.txt's lines for TARGET ("all" lines too);
# STAGE "self" pins amiga-gcc itself, "projects" everything under projects/.
pins() {
    local target=$1 stage=$2 t project commit
    [ "$PINNED" = 1 ] || return 0
    while read -r t project commit; do
        case $t in ''|'#'*) continue ;; esac
        [ "$t" = all ] || [ "$t" = "$target" ] || continue
        if [ "$project" = amiga-gcc ]; then
            [ "$stage" = self ] && { pin . "$commit" || return 1; }
        elif [ "$stage" = projects ] && [ -d "projects/$project" ]; then
            pin "projects/$project" "$commit" || return 1
        fi
    done < "$HERE/toolchain-pins.txt"
}

build() {
    local name=$1 prefix=$2 branch=$3 with_clib2=$4
    local src=~/toolchain-build/src-$name
    log "=== $name: $(date)"
    if [ ! -d "$src" ]; then
        git clone https://codeberg.org/bebbo/amiga-gcc "$src" >"$LOG/$name-clone.log" 2>&1 || {
            log "clone failed"; return 1; }
    fi
    cd "$src" || return 1
    # amiga-gcc's "make update" runs "git pull" in every project, which fails on a
    # detached (pinned) checkout and then retries with ever deeper --depth, forever.
    # So a pinned tree is updated only on its first run, before pinning; and the
    # unpinned mode needs a tree that was never pinned.
    if [ "$PINNED" = 1 ] && [ -d projects/gcc ]; then
        log "$name: pinned tree exists, no 'make update' (delete $src for a fresh one)"
    else
        if [ "$PINNED" != 1 ] && [ -d projects/gcc ] &&
            ! git -C projects/gcc symbolic-ref -q HEAD >/dev/null; then
            log "$name: $src was pinned; PINNED=0 needs a fresh tree: delete it first"
            return 1
        fi
        pins "$name" self || { log "pinning amiga-gcc failed"; return 1; }
        make update >"$LOG/$name-update.log" 2>&1 || { log "update failed"; return 1; }
        if [ -n "$branch" ]; then
            make branch branch="$branch" mod=gcc >"$LOG/$name-branch.log" 2>&1 || {
                log "branch switch failed"; return 1; }
        fi
    fi
    pins "$name" projects || { log "pinning the projects failed"; return 1; }
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
        pins "$name" projects || { log "pinning clib2 failed"; return 1; }
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
