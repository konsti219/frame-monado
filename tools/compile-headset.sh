#!/usr/bin/env bash
set -euo pipefail
build=/work/out/headset
cmake -S /work -B "$build/monado" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/work/tools/aarch64-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/home/steamos/.local/opt/monado \
    -DCMAKE_INSTALL_RPATH=/home/steamos/.local/opt/monado/lib \
    -DXRT_FEATURE_SERVICE=ON -DBUILD_TESTING=OFF -DXRT_FEATURE_DEBUG_GUI=OFF \
    -DXRT_MODULE_MONADO_GUI=OFF -DXRT_BUILD_SAMPLES=OFF
cmake --build "$build/monado" --parallel "$(nproc)"
DESTDIR="$build/stage" cmake --install "$build/monado"
install -Dm755 /usr/lib/aarch64-linux-gnu/libjpeg.so.62 "$build/stage/home/steamos/.local/opt/monado/lib/libjpeg.so.62"
find "$build/stage" -type f \( -name '*.so' -o -name '*.so.*' -o -path "$build/stage/home/steamos/.local/opt/monado/bin/*" \) \
    -exec aarch64-linux-gnu-strip --strip-debug {} +
