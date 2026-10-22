#!/usr/bin/env bash
set -euo pipefail
build=/work/out/headset
ndk=/work/out/android-sdk/android-ndk-r28c
stage="$build/stage/home/steamos/.local/opt/monado"
runtime_tag=$(sed -n 's/^#define GIT_DESC "\(.*\)"/\1/p' "$build/monado/src/xrt/auxiliary/util/u_git_tag.c")
cmake -S /work -B "$build/android-client" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGIT_DESC="$runtime_tag" \
    -DEigen3_DIR=/usr/share/eigen3/cmake \
    -DXRT_FEATURE_SERVICE=OFF -DXRT_FEATURE_CLIENT_WITHOUT_SERVICE=ON \
    -DXRT_FEATURE_ANDROID_HOSTED=ON -DXRT_FEATURE_AHARDWARE_BUFFER=OFF \
    -DXRT_FEATURE_OPENXR=ON -DXRT_FEATURE_DEBUG_GUI=OFF -DXRT_FEATURE_CLIENT_DEBUG_GUI=OFF \
    -DXRT_MODULE_MONADO_GUI=OFF -DXRT_BUILD_SAMPLES=OFF -DBUILD_TESTING=OFF \
    -DXRT_HAVE_OPENGL=OFF -DXRT_HAVE_OPENGL_GLX=OFF \
    -DXRT_FEATURE_OPENXR_LAYER_FB_PASSTHROUGH=ON
cmake --build "$build/android-client" --target openxr_monado --parallel "$(nproc)"
install -Dm755 "$build/android-client/src/xrt/targets/openxr/libopenxr_monado.so" "$stage/lib/android-arm64/libopenxr_monado.so"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-debug "$stage/lib/android-arm64/libopenxr_monado.so"
