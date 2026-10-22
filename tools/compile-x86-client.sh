#!/usr/bin/env bash
set -euo pipefail
build=/work/out/headset
stage="$build/stage/home/steamos/.local/opt/monado"
runtime_tag=$(sed -n 's/^#define GIT_DESC "\(.*\)"/\1/p' "$build/monado/src/xrt/auxiliary/util/u_git_tag.c")
export PKG_CONFIG_LIBDIR=/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig
cmake -S /work -B "$build/x86-client" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGIT_DESC="$runtime_tag" \
    -DXRT_FEATURE_SERVICE=OFF -DXRT_FEATURE_CLIENT_WITHOUT_SERVICE=ON \
    -DXRT_FEATURE_OPENXR=ON -DXRT_FEATURE_DEBUG_GUI=OFF -DXRT_FEATURE_CLIENT_DEBUG_GUI=OFF \
    -DXRT_MODULE_MONADO_GUI=OFF -DXRT_BUILD_SAMPLES=OFF -DBUILD_TESTING=OFF \
    -DXRT_HAVE_OPENGL=OFF -DXRT_HAVE_OPENGL_GLX=OFF -DXRT_HAVE_OPENGLES=OFF -DXRT_HAVE_EGL=OFF
cmake --build "$build/x86-client" --target openxr_monado --parallel "$(nproc)"
install -Dm755 "$build/x86-client/src/xrt/targets/openxr/libopenxr_monado.so" "$stage/lib/x86_64-linux-gnu/libopenxr_monado.so"
strip --strip-debug "$stage/lib/x86_64-linux-gnu/libopenxr_monado.so"
install -Dm644 /work/headset/openxr_frame.json "$stage/share/openxr/1/openxr_frame.json"
