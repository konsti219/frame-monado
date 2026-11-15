# Steam Frame Monado: cross-build in the Podman SDK; frame-shell's installer bundles it, `deploy` updates an install

set shell := ["bash", "-euo", "pipefail", "-c"]

root := justfile_directory()
build_dir := root / "out/headset"
# Inside frame-shell's install (~/.local/share/frame-shell)
prefix := "/home/steamos/.local/share/frame-shell/monado"
stage := build_dir / "stage" + prefix
headset := env("FRAME_HEADSET", "steamos@frame.local")
sdk := "podman run --rm -i --userns=keep-id -v " + root + ":/work:Z -w /work localhost/frame-monado-cross bash -euo pipefail -s"

_default:
    @just --list

# Build and deploy
all: build deploy

# Cross-build the service, the x86-64 client (FEX/Proton) and the Android client (Lepton) into out/headset/stage
build:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{build_dir}}/stage"
    exec 9>"{{build_dir}}/build.lock"
    flock 9
    podman build -t localhost/frame-monado-cross -f "{{root}}/tools/Containerfile" "{{root}}/tools"
    {{sdk}} <<'EOF'
    build=/work/out/headset
    cmake -S /work -B "$build/monado" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/work/tools/aarch64-toolchain.cmake \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX={{prefix}} -DCMAKE_INSTALL_RPATH={{prefix}}/lib \
        -DXRT_FEATURE_SERVICE=ON -DBUILD_TESTING=OFF -DXRT_FEATURE_DEBUG_GUI=OFF \
        -DXRT_MODULE_MONADO_GUI=OFF -DXRT_BUILD_SAMPLES=OFF
    cmake --build "$build/monado" --parallel "$(nproc)"
    DESTDIR="$build/stage" cmake --install "$build/monado"
    install -Dm755 /usr/lib/aarch64-linux-gnu/libjpeg.so.62 "$build/stage{{prefix}}/lib/libjpeg.so.62"
    find "$build/stage" -type f ! -path '*/lib/x86_64-linux-gnu/*' \( -name '*.so' -o -name '*.so.*' -o -path "$build/stage{{prefix}}/bin/*" \) \
        -exec aarch64-linux-gnu-strip --strip-debug {} +
    EOF
    {{sdk}} <<'EOF'
    build=/work/out/headset
    stage="$build/stage{{prefix}}"
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
    EOF
    # Android NDK for the Lepton client (Android games)
    ndk="{{root}}/out/android-sdk/android-ndk-r28c"
    if [[ ! -d "$ndk" ]]; then
        mkdir -p "{{root}}/out/android-sdk"
        curl -fL https://dl.google.com/android/repository/android-ndk-r28c-linux.zip -o "{{root}}/out/android-sdk/ndk.zip"
        echo "a7b54a5de87fecd125a17d54f73c446199e72a64  {{root}}/out/android-sdk/ndk.zip" | sha1sum -c -
        unzip -q "{{root}}/out/android-sdk/ndk.zip" -d "{{root}}/out/android-sdk"
        rm "{{root}}/out/android-sdk/ndk.zip"
    fi
    {{sdk}} <<'EOF'
    build=/work/out/headset
    ndk=/work/out/android-sdk/android-ndk-r28c
    stage="$build/stage{{prefix}}"
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
    EOF

# Update Monado and the session files in a frame-shell install, restarting Monado if it ran; FRAME_HEADSET overrides steamos@frame.local
deploy:
    #!/usr/bin/env bash
    set -euo pipefail
    [ -x "{{stage}}/bin/monado-service" ] || { echo "Run just build first" >&2; exit 1; }
    headset="{{headset}}"
    ssh "$headset" '[ -x ~/.local/share/frame-shell/install.sh ]' || { echo "Install frame-shell on the headset first" >&2; exit 1; }
    ssh "$headset" 'mkdir -p ~/.local/state/frame-monado/deploy/monado ~/.local/state/frame-monado/deploy/session'
    rsync -a --delete "{{stage}}/" "$headset:.local/state/frame-monado/deploy/monado/"
    rsync -a --delete "{{root}}/headset/" "$headset:.local/state/frame-monado/deploy/session/"
    ssh -t "$headset" '~/.local/share/frame-shell/install.sh monado ~/.local/state/frame-monado/deploy'
