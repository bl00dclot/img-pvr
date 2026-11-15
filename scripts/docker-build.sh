#!/usr/bin/env bash
# Build the driver in docker/Dockerfile's image. Usage: scripts/docker-build.sh <android-ndk-dir>
# Env: ARCHES (default "armv7"), DDK (1.11 default, or 1.13). The NDK is mounted read-only; outputs land in this tree (build-*/, dist/).
set -euo pipefail
NDK=${1:?usage: scripts/docker-build.sh <android-ndk-dir>}
[ -d "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin" ] || { echo "error: not an NDK: $NDK" >&2; exit 1; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=img-pvr-build:trixie
# Reproducible output: build timestamps come from the commit, not the clock.
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}
docker build -q -t "$IMAGE" "$ROOT/docker" >/dev/null
docker run --rm --cap-drop ALL --security-opt no-new-privileges --user "$(id -u):$(id -g)" -e HOME=/tmp -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" -e ARCHES="${ARCHES:-armv7}" -e DDK="${DDK:-1.11}" \
   -v "$ROOT:/src" -v "$ROOT/.git:/src/.git:ro" -v "$NDK:/ndk:ro" "$IMAGE" ./build.sh /ndk
