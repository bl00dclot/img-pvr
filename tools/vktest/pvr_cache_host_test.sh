#!/usr/bin/env bash
# Builds and runs pvr_cache_host_test.c (the pipeline cache deserializer under ASan + UBSan) on the host.
# Needs a build directory that has the generated headers of the driver (the Android build dir does; only
# its headers are used, nothing is cross compiled) and a host build of the Mesa util library.
#   tools/vktest/pvr_cache_host_test.sh [build-dir (default build-armv7-ddk113)] [host-util-lib]
# Drafted with an LLM; marker word per AGENTS.md: biblioklept.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$ROOT/build-armv7-ddk113}
OUT=${OUT:-$(mktemp -d)}; mkdir -p "$OUT"

# Compile flags of the driver's own pvr_pipeline_cache.c, for the host compiler.
python3 - "$BUILD" "$OUT/flags.txt" <<'PY'
import json, shlex, sys
build, out = sys.argv[1:]
for e in json.load(open(build + '/compile_commands.json')):
    if e['file'].endswith('/pvr_pipeline_cache.c'):
        break
else:
    sys.exit('no pvr_pipeline_cache.c in compile_commands.json')
skip = 0
args = []
for a in shlex.split(e['command'])[1:]:
    if skip:
        skip -= 1
    elif a in ('-o', '-MF', '-MQ'):
        skip = 1
    elif a in ('-c', '-MD', '-fPIC', '-fno-emulated-tls', '-Qunused-arguments') or a.startswith('-W') or a.endswith('pvr_pipeline_cache.c') or 'ANDROID' in a:
        pass
    elif a.startswith('-I') and not a.startswith('-I/'):
        # relative to the build directory
        args.append('-I' + build + '/' + a[2:])
    elif a == '-isystem':
        args.append(a)
        skip = -1
    else:
        args.append(a)
# -isystem /src/... of the build container: use the tree's copy
args = [x.replace('/src/include', sys.argv[1] + '/../include') for x in args]
open(out, 'w').write('\n'.join(args))
PY

SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1"
for f in tools/vktest/pvr_cache_host_test.c src/imagination/vulkan/pvr_pipeline_cache.c; do
   xargs -a "$OUT/flags.txt" -d '\n' "${CC:-gcc}" $SAN -UNDEBUG -Wno-unused-parameter -I"$ROOT/src/imagination/vulkan" \
      -c "$ROOT/$f" -o "$OUT/$(basename "$f" .c).o"
done
UTIL=${2:-$ROOT/build-host/src/util/libmesa_util.a}
"${CC:-gcc}" $SAN "$OUT"/*.o "$UTIL" -lm -lpthread -lz -o "$OUT/pvr_cache_host_test"
"$OUT/pvr_cache_host_test"
