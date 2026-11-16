#!/usr/bin/env bash
# XCLIPSE: regression test for the legalisation of a backend instruction whose destination is a pixel output
# register (smp -> po0). With PCO_DEBUG=fuse_all the load-op shader's smp result is fused into po0; legalisation must
# keep the write to po0 after the smp (and its wdf). Before the fix the copy was put before the smp and po0 was
# never written, so the attachment load read garbage.
# usage: test_smp_po0.sh <vkbench> <libvulkan_powervr.so> <expected load sum>      (run on the device, vkbench as in tools/vktest)
set -euo pipefail
vkbench=${1:?vkbench} so=${2:?driver} want=${3:?expected sum of vkbench load}
out=$(mktemp); trap 'rm -f "$out"' EXIT
got=$(PCO_DEBUG=fuse_all "$vkbench" "$so" load 20 2>&1 | sed -n 's/.*sum \([0-9]*\).*/\1/p')
[ "$got" = "$want" ] || { echo "FAIL: load sum $got, expected $want"; exit 1; }
PCO_DEBUG=fuse_all PCO_DEBUG_PRINT=fs,internal "$vkbench" "$so" load 1 > "$out" 2>&1 </dev/null || true
# every internal shader (after passes) that has a backend smp must write a pixel output register after it
awk '/^shader ir after passes:/ {inblk=1; smp=0; next}
     /^shader ir before passes:/ {inblk=0}
     inblk && /backend: smp/ {smp=NR; n++}
     inblk && smp && /w0 = po/ {smp=0; ok++}
     inblk && /^}/ {if (smp) {print "FAIL: smp without a later po write, line " smp; bad=1} smp=0}
     END {if (!n) {print "FAIL: no smp found"; exit 1} if (bad) exit 1; print "ok: " ok " of " n " smp shaders write po after the smp"}' "$out"
