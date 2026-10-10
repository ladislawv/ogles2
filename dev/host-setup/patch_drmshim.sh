#!/bin/bash
# WSL1 has no memfd_create(); let drm-shim fall back to an unlinked temp file.
set -e
F=~/mesa-build/mesa/src/drm-shim/device.c
if ! grep -q "WSL1 fallback" "$F"; then
python3 - "$F" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '   shim_device.mem_fd = memfd_create("shim mem", MFD_CLOEXEC);\n'
new = (old +
       '   if (shim_device.mem_fd == -1) {\n'
       '      /* WSL1 fallback: no memfd_create(), use an unlinked temp file. */\n'
       '      char tmpl[] = "/tmp/drm-shim-mem-XXXXXX";\n'
       '      shim_device.mem_fd = mkstemp(tmpl);\n'
       '      if (shim_device.mem_fd != -1)\n'
       '         unlink(tmpl);\n'
       '   }\n')
assert old in s
s = s.replace(old, new)
if '#include <stdlib.h>' not in s:
    s = '#include <stdlib.h>\n' + s
if '#include <unistd.h>' not in s:
    s = '#include <unistd.h>\n' + s
open(p, 'w').write(s)
print("patched")
PY
fi
cd ~/mesa-build/mesa
git diff --stat
. ~/mesa-build/venv/bin/activate
ninja -C build -j24 >~/mesa-build/rebuild.log 2>&1 && ninja -C build install >>~/mesa-build/rebuild.log 2>&1 && echo rebuilt
git diff > ~/mesa-build/drm-shim-wsl1.patch
