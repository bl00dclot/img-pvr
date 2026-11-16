#!/usr/bin/env bash
# Compile the corpus (corpus.txt) to SPIR-V and run pco_stats on every pipeline in the build image.
# usage: tools/pco-stats/run.sh [-b build-host-dir] [-p probe-shaders-dir] [-g glslc] out.txt
# Needs build-host/src/imagination/pco/pco_stats (scripts, see README.md). Probe lines are skipped without -p/-g.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=build-host PROBE= GLSLC=
while getopts b:p:g: o; do case $o in b) BUILD=$OPTARG;; p) PROBE=$OPTARG;; g) GLSLC=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND - 1))
OUT=${1:?usage: run.sh [-b build-host-dir] [-p probe-shaders-dir] [-g glslc] out.txt}
IMAGE=img-pvr-build:trixie
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
# The probe's shaders are built by the app with glslc -O; do the same on the host.
if [ -n "$PROBE" ] && [ -n "$GLSLC" ]; then
   for f in "$PROBE"/*.vert "$PROBE"/*.frag; do "$GLSLC" -O -o "$W/probe_$(basename "$f").spv" "$f"; done
fi
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges --user "$(id -u):$(id -g)" \
   -e HOME=/tmp -e BUILD="$BUILD" -e PCO_DEBUG -v "$ROOT:/src:ro" -v "$W:/w" "$IMAGE" bash -c '
set -euo pipefail
for f in /src/tools/vktest/bench/*.vert /src/tools/vktest/bench/*.frag; do
   glslangValidator -V -o "/w/bench_$(basename "$f").spv" "$f" >/dev/null
done
while read -r name vs fs flags; do
   case "$name" in ""|"#"*) continue;; esac
   v=/w/${vs%%:*}_${vs#*:}.spv; f=/w/${fs%%:*}_${fs#*:}.spv
   [ -f "$f" ] || { echo "skip $name (no $f)" >&2; continue; }
   /src/$BUILD/src/imagination/pco/pco_stats $flags "$name" "$v" "$f"
done < /src/tools/pco-stats/corpus.txt > /w/out.txt'
cp "$W/out.txt" "$OUT"
