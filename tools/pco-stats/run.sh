#!/usr/bin/env bash
# Compile the corpus (corpus.txt) to SPIR-V and run pco_stats on every pipeline in the build image.
# usage: tools/pco-stats/run.sh [-b build-host-dir] [-p probe-shaders-dir] [-g glslc] [-c extra-corpus-dir] out.txt
# -c DIR: a private corpus kept outside the repository: DIR/corpus.txt (same format; `extra:name` names DIR/spv/name.spv)
# is run after corpus.txt. A pipeline that fails (pco_stats exits non-zero, e.g. input attachments) is reported on stderr and left out.
# ASAN_OPTIONS, UBSAN_OPTIONS and NIR_DEBUG of the caller reach pco_stats (for a sanitizer/validation build, -b build-dir).
# Needs build-host/src/imagination/pco/pco_stats (scripts, see README.md). Probe lines are skipped without -p/-g.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=build-host PROBE= GLSLC= EXTRA=
while getopts b:p:g:c: o; do case $o in b) BUILD=$OPTARG;; p) PROBE=$OPTARG;; g) GLSLC=$OPTARG;; c) EXTRA=$(cd "$OPTARG" && pwd);; *) exit 2;; esac; done
shift $((OPTIND - 1))
OUT=${1:?usage: run.sh [-b build-host-dir] [-p probe-shaders-dir] [-g glslc] [-c extra-corpus-dir] out.txt}
IMAGE=${IMAGE:-img-pvr-build:trixie} # IMAGE=img-pvr-build:trixie-san for a build-dir with sanitizers
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
# The probe's shaders are built by the app with glslc -O; do the same on the host.
if [ -n "$PROBE" ] && [ -n "$GLSLC" ]; then
   for f in "$PROBE"/*.vert "$PROBE"/*.frag; do "$GLSLC" -O -o "$W/probe_$(basename "$f").spv" "$f"; done
fi
# An extra corpus brings its own SPIR-V (spv/*.spv), copied as extra_<file> so that `extra:file` names it.
MOUNT=()
if [ -n "$EXTRA" ]; then
   for f in "$EXTRA"/spv/*.spv; do cp "$f" "$W/extra_$(basename "$f")"; done
   MOUNT=(-v "$EXTRA:/extra:ro")
fi
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges --user "$(id -u):$(id -g)" \
   -e HOME=/tmp -e BUILD="$BUILD" -e EXTRA="$EXTRA" -e ASAN_OPTIONS -e UBSAN_OPTIONS -e NIR_DEBUG -e PCO_DEBUG -e PCO_UBO_PUSH_BUDGET -v "$ROOT:/src:ro" -v "$W:/w" "${MOUNT[@]}" "$IMAGE" bash -c '
set -euo pipefail
for f in /src/tools/vktest/bench/*.vert /src/tools/vktest/bench/*.frag; do
   glslangValidator -V -o "/w/bench_$(basename "$f").spv" "$f" >/dev/null
done
# shaders only the statistics tool uses, not vkbench
for f in /src/tools/pco-stats/shaders/*.frag /src/tools/pco-stats/shaders/*.vert; do
   glslangValidator -V -o "/w/stats_$(basename "$f").spv" "$f" >/dev/null
done
while read -r name vs fs flags; do
   case "$name" in ""|"#"*) continue;; esac
   v=/w/${vs%%:*}_${vs#*:}.spv; f=/w/${fs%%:*}_${fs#*:}.spv
   [ -f "$f" ] || { echo "skip $name (no $f)" >&2; continue; }
   /src/$BUILD/src/imagination/pco/pco_stats $flags "$name" "$v" "$f" || echo "FAIL $name (exit $?)" >&2
done < <(cat /src/tools/pco-stats/corpus.txt ${EXTRA:+/extra/corpus.txt}) > /w/out.txt'
cp "$W/out.txt" "$OUT"
