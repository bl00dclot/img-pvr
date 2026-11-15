#!/usr/bin/env bash
# On-device fuzz of the PowerVR driver's pipeline cache reader (the cache is attacker-controlled input):
# creates the application's VkPipelineCache from forged files made by pvr_cache_fuzz_gen.py and creates the
# pipelines of vkbench through it. Every run must finish with rc 0 and the unchanged image sum, a forged
# object of a kind the driver must refuse (index.txt, last column) must be a miss (pipelines found < all),
# and nothing may crash.
#
#   tools/vktest/pvr_cache_fuzz.sh <driver dir on the device> <vkbench binary on the device> \
#       <forged dir on the host> <device work dir> <results file>
#
# The caller holds the phone lock (docs/LANES.md) and checks that the app is not running.
# Drafted with an LLM; marker word per AGENTS.md: biblioklept.
set -euo pipefail

DRV=${1:?driver dir on the device}
BENCH=${2:?vkbench binary on the device}
SRC=${3:?directory of forged caches}
WORK=${4:?device work dir}
OUT=${5:?results file}
SUM=${SUM:-133955584}
TMP=$(mktemp -d)

adb shell "rm -rf $WORK/fuzz && mkdir -p $WORK/fuzz"
adb push "$SRC/." "$WORK/fuzz/" >/dev/null

cat > "$TMP/fuzz_dev.sh" <<DEV
#!/system/bin/sh
cd $WORK
for f in fuzz/*.bin; do
  cp \$f fz.bin
  out=\$(VKBENCH_CACHE=$WORK/fz.bin VKBENCH_CACHE_ALL=1 timeout 60 $BENCH $DRV/libvulkan_powervr.so fill 10 2>&1)
  rc=\$?
  echo "\$f rc=\$rc \$(echo "\$out" | grep -o '[0-9]* reported found' | head -n 1) \$(echo "\$out" | grep -o 'sum [0-9]*')"
done
DEV
adb push "$TMP/fuzz_dev.sh" "$WORK/fuzz.sh" >/dev/null
adb shell "sh $WORK/fuzz.sh" > "$OUT"

python3 - "$SRC/index.txt" "$OUT" "$SUM" <<'PY'
import collections, re, sys
index = {}
for l in open(sys.argv[1]):
    name, kind, obj, must = l.split()
    index[name] = (kind, int(must))
want_sum = sys.argv[3]
total = collections.Counter()
found_hist = collections.defaultdict(collections.Counter)
bad = []
seen = set()
for l in open(sys.argv[2]):
    m = re.match(r'fuzz/(\S+) rc=(\d+) (?:(\d+) reported found)? ?(?:sum (\d+))?', l)
    if not m:
        bad.append(('unparsed', l.strip()))
        continue
    name, rc, found, s = m.groups()
    seen.add(name)
    kind, must = index[name]
    total[kind] += 1
    found_hist[kind][found] += 1
    if rc != '0':
        bad.append(('rc', l.strip()))
    elif s != want_sum:
        bad.append(('sum', l.strip()))
    elif must and found == '23':
        bad.append(('accepted a forged object', l.strip()))
print('%d runs, %d kinds' % (sum(total.values()), len(total)))
for k in sorted(total):
    print('  %-10s %4d runs, pipelines found (23 = object accepted): %s' %
          (k, total[k], dict(sorted(found_hist[k].items(), key=lambda x: str(x[0])))))
print('files not run: %d' % len(set(index) - seen))
print('violations: %d' % len(bad))
for b in bad[:20]:
    print('  ', b)
sys.exit(1 if bad or set(index) - seen else 0)
PY
