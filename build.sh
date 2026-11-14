#!/bin/sh
# Build the PowerVR Vulkan driver for Android (API 29) and package it into dist/.
#
# usage: ./build.sh [path-to-android-ndk]
#   The NDK can also come from ANDROID_NDK_HOME or ANDROID_NDK_ROOT. Linux only (WSL works): the
#   first stage builds host tools that need LLVM.
#   ARCHES picks the targets (default: "armv7 arm64"); each builds in build-<arch>.
#
# Two stages:
#   1. build-host: mesa_clc, vtn_bindgen2, pco_clc and pdsc_precomp, native, from this tree. They
#      compile the driver's built-in shader and PDS programs, so they have to be built from the
#      same compiler sources as the driver.
#   2. build-<arch>: libvulkan_powervr_mesa.so, cross-compiled with the NDK, using those tools.
#
# Requires: meson, ninja, python3 (mako, packaging, pyyaml), pkg-config, flex, bison, and for
# stage 1 LLVM with clang, libclc and the SPIR-V LLVM translator of one major version (Ubuntu:
# llvm-dev libclang-dev libclc-XX-dev libllvmspirvlib-XX-dev clang).
set -eu

cd "$(dirname "$0")"

NDK="${1:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
   echo "error: pass the Android NDK path or set ANDROID_NDK_HOME" >&2
   exit 1
fi

case "$(uname -s)" in
   Linux*) HOST=linux-x86_64 ;;
   *) echo "error: build on Linux (or WSL); stage 1 needs LLVM" >&2; exit 1 ;;
esac

BIN="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
ARCHES="${ARCHES:-armv7 arm64}"
# DDK picks the kernel driver version the driver speaks: 1.11 (default, build-<arch>) or 1.13
# (build-<arch>-ddk113, zip suffix -ddk113).
DDK="${DDK:-1.11}"
case "$DDK" in
   1.11) DDK_SUFFIX= ;;
   1.13) DDK_SUFFIX=-ddk113 ;;
   *) echo "error: DDK must be 1.11 or 1.13" >&2; exit 1 ;;
esac
export PVR_DDK="$DDK"

for tool in meson ninja python3 pkg-config flex bison; do
   if ! command -v "$tool" >/dev/null 2>&1; then
      echo "error: $tool not found in PATH" >&2
      exit 1
   fi
done

# --- stage 1: host tools ----------------------------------------------------------------------
HOSTB=build-host
if [ ! -f "$HOSTB/build.ninja" ]; then
   meson setup "$HOSTB" \
      -Dbuildtype=release \
      -Dmesa-clc=enabled -Dprecomp-compiler=enabled \
      -Dvulkan-drivers=imagination -Dgallium-drivers= -Dplatforms= -Dllvm=enabled \
      -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
      -Dtools= -Dvideo-codecs= \
      -Dallow-fallback-for=libdrm --force-fallback-for=libdrm,expat,zlib \
      -Dlibdrm:default_library=static
fi
ninja -C "$HOSTB" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 \
   src/imagination/pco/uscgen/pco_clc src/imagination/pdsc/pdsc_precomp
mkdir -p "$HOSTB/bin"
cp "$HOSTB/src/compiler/clc/mesa_clc" "$HOSTB/src/compiler/spirv/vtn_bindgen2" \
   "$HOSTB/src/imagination/pco/uscgen/pco_clc" "$HOSTB/src/imagination/pdsc/pdsc_precomp" \
   "$HOSTB/bin/"
PATH="$PWD/$HOSTB/bin:$PATH"
export PATH

# --- stage 2: the driver ----------------------------------------------------------------------
LIB=src/imagination/vulkan/libvulkan_powervr_mesa.so
for arch in $ARCHES; do
   case "$arch" in
      armv7) CC=armv7a-linux-androideabi29 FAMILY=arm CPU=armv7 ;;
      arm64) CC=aarch64-linux-android29 FAMILY=aarch64 CPU=aarch64 ;;
      *) echo "error: unknown arch $arch (armv7 or arm64)" >&2; exit 1 ;;
   esac
   if [ ! -x "$BIN/$CC-clang" ]; then
      echo "error: no $CC compiler in $BIN" >&2
      exit 1
   fi

   BUILD="build-$arch$DDK_SUFFIX"
   mkdir -p "$BUILD" "$BUILD/pkgconfig-empty"
   cat > "$BUILD/cross.ini" <<EOF
[binaries]
c = '$BIN/$CC-clang'
cpp = '$BIN/$CC-clang++'
ar = '$BIN/llvm-ar'
strip = '$BIN/llvm-strip'
pkg-config = 'pkg-config'

[properties]
# Keep the host's .pc files out of the cross build.
pkg_config_libdir = ['$PWD/$BUILD/pkgconfig-empty']
cpp_link_args = ['-static-libstdc++']

[host_machine]
system = 'android'
cpu_family = '$FAMILY'
cpu = '$CPU'
endian = 'little'
EOF

   if [ ! -f "$BUILD/build.ninja" ]; then
      meson setup "$BUILD" --cross-file "$BUILD/cross.ini" \
         -Dbuildtype=debugoptimized -Db_ndebug=true \
         -Dplatforms=android -Dplatform-sdk-version=29 -Dandroid-stub=true -Dandroid-strict=false \
         -Dgallium-drivers= -Dvulkan-drivers=imagination -Dimagination-srv=true \
         -Dimagination-srv-ddk="$DDK" \
         -Dmesa-clc=system -Dprecomp-compiler=system -Dllvm=disabled -Dzstd=disabled \
         -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled \
         -Dgles2=disabled -Dvulkan-layers= -Dtools= -Dperfetto=false -Dlibunwind=disabled \
         -Dallow-fallback-for=libdrm --force-fallback-for=expat,libdrm,zlib \
         -Dlibdrm:default_library=static -Dexpat:default_library=static -Dzlib:default_library=static
   fi
   ninja -C "$BUILD" "$LIB"

   mkdir -p "$BUILD/stripped"
   "$BIN/llvm-strip" --strip-unneeded -o "$BUILD/stripped/libvulkan_powervr.so" "$BUILD/$LIB"
   python3 android/package.py "$BUILD/stripped/libvulkan_powervr.so" "$arch" dist
done
