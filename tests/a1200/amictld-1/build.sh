#!/bin/bash
# Fills the ogles2-amictld-1 package directory ($1): the remote test station's
# server, its token and its allow list (from tools/amictl/station.local, which
# stays on this PC and out of git). Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}

make -C "$REPO" -j8 B="$B" "$B/amictl/amictld" > /dev/null
cp "$B/amictl/amictld" "$PKG/"
python3 - "$REPO/tools/amictl/station.local" "$PKG" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1]))
open(sys.argv[2] + "/amictld.token", "w").write(cfg["token"] + "\n")
open(sys.argv[2] + "/amictld.allow", "w").write("\n".join(cfg["allow"]) + "\n")
PY
