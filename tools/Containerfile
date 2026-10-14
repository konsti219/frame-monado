FROM registry.gitlab.steamos.cloud/proton/steamrt4/sdk/x86_64:4.0.20260331.220802-0

RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y --no-install-recommends \
    crossbuild-essential-arm64 clang libclang-dev cmake ninja-build pkg-config python3-jinja2 \
    glslang-tools spirv-tools librsvg2-bin libshaderc-dev libeigen3-dev \
    libvulkan-dev:arm64 libwayland-dev:arm64 libxkbcommon-dev:arm64 libinput-dev:arm64 \
    libudev-dev:arm64 libdbus-1-dev:arm64 libpipewire-0.3-dev:arm64 libpulse-dev:arm64 \
    libopenxr-dev:arm64 libssl-dev:arm64 libsystemd-dev:arm64 \
    libusb-1.0-0-dev:arm64 libhidapi-dev:arm64 libjpeg-dev:arm64 libcjson-dev:arm64 \
    libegl-dev:arm64 libgles-dev:arm64 libgl-dev:arm64 libx11-dev:arm64 libxrandr-dev:arm64 \
    libxcb-randr0-dev:arm64 libx11-xcb-dev:arm64 libbluetooth-dev:arm64 libsdl2-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fL https://github.com/KhronosGroup/KTX-Software/releases/download/v4.4.2/KTX-Software-4.4.2-Linux-arm64.deb -o /tmp/ktx-arm64.deb \
    && echo '654e9fb9323e1fe89255939952c4f4867692ab1f4e8f71df2b4e04de1802b113  /tmp/ktx-arm64.deb' | sha256sum -c - \
    && dpkg-deb -x /tmp/ktx-arm64.deb /opt/ktx-arm64 && rm /tmp/ktx-arm64.deb
RUN curl -fL https://github.com/KhronosGroup/KTX-Software/releases/download/v4.4.2/KTX-Software-4.4.2-Linux-x86_64.deb -o /tmp/ktx-host.deb \
    && dpkg -i /tmp/ktx-host.deb && rm /tmp/ktx-host.deb

ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain 1.99.0 \
    && /opt/cargo/bin/rustup target add aarch64-unknown-linux-gnu
ENV PATH=/opt/cargo/bin:$PATH
ENV CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc
ENV CC_aarch64_unknown_linux_gnu=aarch64-linux-gnu-gcc CXX_aarch64_unknown_linux_gnu=aarch64-linux-gnu-g++
ENV PKG_CONFIG_ALLOW_CROSS=1 PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
ENV SHADERC_LIB_DIR=/usr/lib/x86_64-linux-gnu
RUN apt-get update && apt-get install -y --no-install-recommends libcurl4-openssl-dev:arm64 libpng-dev:arm64 libasound2-dev:arm64 libfontconfig-dev:arm64 libharfbuzz-dev:arm64 libfreetype-dev:arm64 && rm -rf /var/lib/apt/lists/*
RUN apt-get update && apt-get install -y --no-install-recommends libopengl-dev:arm64 libdav1d-dev:arm64 && rm -rf /var/lib/apt/lists/*
