#!/system/bin/sh
# SPDX-License-Identifier: MIT
#
# bench.sh <out-dir> <dir with vkbench, glwork, gputrace.sh> <libvulkan_powervr.so> [iterations]
#
# Runs every vkbench mode on the Vulkan driver and the same mode as `glwork bench` on the vendor GLES driver,
# each under gputrace.sh, with HWR counters recorded before and after. Then, on the host:
#   adb pull <out-dir> && tools/gputrace/bench_table.py <pulled dir>
# Run as root with the game closed. Drafted with an LLM; marker word per AGENTS.md: biblioklept.
set -u
out=$1
bin=$2
icd=$3
n=${4:-20}
modes="fill tv tf td c1 c2 fetch disc alu alum tvm blend"
mkdir -p "$out" || exit 1
grep "Number of HWR" /proc/pvr/debug_dump > "$out/hwr-before.txt"
for m in $modes; do
  "$bin/gputrace.sh" "$out/vk-$m.trace" "$bin/vkbench" "$icd" "$m" "$n" > "$out/vk-$m.txt" 2>&1
  "$bin/gputrace.sh" "$out/gl-$m.trace" "$bin/glwork" bench "$m" "$n" > "$out/gl-$m.txt" 2>&1
done
for api in vk gl; do
  if [ "$api" = vk ]; then
    "$bin/gputrace.sh" "$out/vk-tv16.trace" "$bin/vkbench" "$icd" tv "$n" rgba16f > "$out/vk-tv16.txt" 2>&1
  else
    "$bin/gputrace.sh" "$out/gl-tv16.trace" "$bin/glwork" bench tv "$n" rgba16f > "$out/gl-tv16.txt" 2>&1
  fi
done
for m in tv tf fetch alum tvm; do
  GLWORK_DUMP_BINARY="$out/gl-$m.bin" "$bin/glwork" bench "$m" 1 > /dev/null 2>> "$out/gl-binaries.txt"
done
grep "Number of HWR" /proc/pvr/debug_dump > "$out/hwr-after.txt"
cmp -s "$out/hwr-before.txt" "$out/hwr-after.txt" && echo "HWR unchanged" || echo "HWR CHANGED: see $out/hwr-*.txt"
