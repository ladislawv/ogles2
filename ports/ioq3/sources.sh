# The Quake III sources ports/ioq3 and ports/ioq3-gl2 build from; sourced by
# their build.sh. Each is a clone at a pinned commit in ~, made on first use:
#   IOQ3_SRC     the AmigaOS 3 engine: Cowcat's ioquake3 port with the
#                PiStorm changes (github.com/ladislawv/ioq3-amiga)
#   IOQ3_UP_SRC  upstream ioquake3 (github.com/ioquake/ioq3), for
#                renderergl2 (ports/ioq3-gl2 only)
# Set either to a tree of your own (a working copy with changes, say) and it
# is used as it is: nothing is cloned or checked out there.

IOQ3_AMIGA_URL=${IOQ3_AMIGA_URL:-https://github.com/ladislawv/ioq3-amiga.git}
IOQ3_AMIGA_COMMIT=1598c28ed6bd69095576ef9f03a1329a20ccfd2a # build 296
IOQ3_UP_URL=${IOQ3_UP_URL:-https://github.com/ioquake/ioq3.git}
IOQ3_UP_COMMIT=83a776283bdb958f82db25554b5ed0966aaf6e49 # 2026-09-17

# clone_pinned DIR URL COMMIT: DIR at COMMIT, cloned or fetched as needed.
clone_pinned() {
    local dir=$1 url=$2 commit=$3
    # No password prompt: a build must not wait on one (a private repository
    # needs git credentials set up in WSL, see CONTRIBUTING.md).
    if [ ! -d "$dir/.git" ]; then
        GIT_TERMINAL_PROMPT=0 git clone -q "$url" "$dir" ||
            { echo "could not clone $url (a private repository needs git credentials in WSL)"; rm -rf "$dir"; return 1; }
    fi
    if ! git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null; then
        GIT_TERMINAL_PROMPT=0 git -C "$dir" fetch -q origin || { echo "could not fetch $url"; return 1; }
    fi
    git -C "$dir" -c advice.detachedHead=false checkout -q "$commit" || { echo "$dir: no commit $commit"; return 1; }
}

if [ -z "${IOQ3_SRC:-}" ]; then
    IOQ3_SRC=$HOME/ioq3-amiga
    clone_pinned "$IOQ3_SRC" "$IOQ3_AMIGA_URL" "$IOQ3_AMIGA_COMMIT" || exit 1
fi
if [ -n "${IOQ3_WANT_UPSTREAM:-}" ] && [ -z "${IOQ3_UP_SRC:-}" ]; then
    IOQ3_UP_SRC=$HOME/ioq3-upstream
    clone_pinned "$IOQ3_UP_SRC" "$IOQ3_UP_URL" "$IOQ3_UP_COMMIT" || exit 1
fi
