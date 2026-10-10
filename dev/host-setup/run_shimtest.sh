#!/bin/bash
# Builds and runs shimtest.c against ~/mesa-install with the v3d drm-shim (V3D 4.2).
M=$HOME/mesa-install
L=$M/lib/x86_64-linux-gnu
SRC="$(dirname "$(readlink -f "$0")")/shimtest.c"
mkdir -p ~/stream-b
gcc -O1 -I"$M/include" "$SRC" -L"$L" -Wl,-rpath-link,"$L" -Wl,-rpath,"$L" -lEGL -lGLESv2 -o ~/stream-b/shimtest || exit 1
echo "=== v3d via drm-shim (V3D_GPU_ID=42), V3D_DEBUG=qpu"
LD_LIBRARY_PATH="$L" LIBGL_DRIVERS_PATH="$L/dri" \
  LD_PRELOAD="$L/libv3d_noop_drm_shim.so" V3D_GPU_ID=42 MESA_LOADER_DRIVER_OVERRIDE=v3d \
  V3D_DEBUG=qpu ~/stream-b/shimtest 2>&1 | head -60
echo "=== llvmpipe"
LD_LIBRARY_PATH="$L" LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe ~/stream-b/shimtest 2>&1 | head -12
