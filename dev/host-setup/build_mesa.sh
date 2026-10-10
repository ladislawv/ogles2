#!/bin/bash
# Host Mesa for track B (offline shader compiler v3dglslc) and for reference images.
#   - gallium v3d driver + v3d drm-shim  -> compile V3D 4.2 shaders with no Pi (V3D_GPU_ID=42)
#   - llvmpipe                            -> software-rendered GLES2 reference images on this PC
# Installs into ~/mesa-install. Logs in ~/mesa-build/*.log.
set -u
MESA_COMMIT=${MESA_COMMIT:-74b6971}
W=~/mesa-build
mkdir -p "$W"
cd "$W" || exit 1

python3 -m venv "$W/venv" >"$W/venv.log" 2>&1
. "$W/venv/bin/activate"
pip install -q meson mako pyyaml packaging >"$W/pip.log" 2>&1

if [ ! -d mesa ]; then
    git clone https://gitlab.freedesktop.org/mesa/mesa.git mesa >"$W/clone.log" 2>&1 || { echo "clone failed"; exit 1; }
fi
cd mesa
git fetch -q origin "$MESA_COMMIT" 2>/dev/null
git checkout -q "$MESA_COMMIT" || { echo "checkout $MESA_COMMIT failed"; exit 1; }
git log -1 --format='mesa %h %ad %s' --date=short | tee "$W/summary.log"

meson setup build --prefix="$HOME/mesa-install" --buildtype=debugoptimized \
    -Dgallium-drivers=v3d,llvmpipe -Dvulkan-drivers= \
    -Dplatforms= -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dglx=disabled \
    -Dllvm=enabled -Dtools=drm-shim -Dbuild-tests=false \
    >"$W/setup.log" 2>&1 || { echo "meson setup failed, see $W/setup.log" | tee -a "$W/summary.log"; exit 1; }
ninja -C build -j24 >"$W/build.log" 2>&1 || { echo "ninja failed, see $W/build.log" | tee -a "$W/summary.log"; exit 1; }
ninja -C build install >"$W/install.log" 2>&1
echo "MESA DONE $(date)" | tee -a "$W/summary.log"
find "$HOME/mesa-install" -name 'libv3d_noop_drm_shim.so' -o -name 'libEGL.so.1' | tee -a "$W/summary.log"
