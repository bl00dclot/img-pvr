# Reproducible build environment for build.sh (host tools + Android cross build).
# Base pinned by digest; packages from the Debian trixie archive only. The NDK is mounted at run time.
FROM debian@sha256:9c6e381421d0f48d1b8133e573f33c185ee16cbf505eda9c4a8a4791b25ea8a6
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      clang-19 llvm-19-dev libclang-19-dev libclang-cpp19-dev libclc-19 libclc-19-dev \
      libllvmspirvlib-19-dev llvm-spirv-19 spirv-tools glslang-tools \
      python3 python3-mako python3-yaml python3-packaging flex bison pkg-config meson ninja-build \
      zlib1g-dev libzstd-dev libexpat1-dev git ca-certificates \
   && rm -rf /var/lib/apt/lists/*
ENV LLVM_CONFIG=llvm-config-19 PATH=/usr/lib/llvm-19/bin:$PATH
WORKDIR /src
