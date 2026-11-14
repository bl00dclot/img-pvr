#!/usr/bin/env bash
# Check that pvr_srv_bo.h's memory allocation flags equal the kernel's own values for one DDK.
# Usage: tools/check_memflags.sh <111|113> <kernel gpu_rgx/m1.xxEDyyyy dir>
# Builds a host program from the kernel's pvrsrv_memallocflags.h / rgx_memallocflags.h and the
# flag block of pvr_srv_bo.h, and compares 26 values. Exit status = number of mismatches.
set -euo pipefail
DDK=${1:?usage: check_memflags.sh <111|113> <km dir>}
KM=${2:?usage: check_memflags.sh <111|113> <km dir>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
awk '/MemAlloc flags/{p=1;next} /struct definitions/{p=0} p' \
   "$ROOT/src/imagination/vulkan/winsys/pvrsrvkm/pvr_srv_bo.h" | grep -vE '^/\*+$|^ \*+/$' > "$WORK/flags.h"
PAIRS="DEVICE_FLAGS_OFFSET DEVICE_FLAGS_MASK CPU_CACHE_CLEAN KERNEL_CPU_MAPPABLE ZERO_ON_ALLOC SVM_ALLOC
POISON_ON_ALLOC POISON_ON_FREE GPU_READABLE GPU_WRITEABLE GPU_CACHE_MODE_MASK CPU_READABLE CPU_WRITEABLE
CPU_CACHE_MODE_MASK CPU_CACHE_INCOHERENT NO_OSPAGES_ON_ALLOC SPARSE_NO_DUMMY_BACKING SPARSE_ZERO_BACKING
GPU_UNCACHED GPU_CACHE_INCOHERENT CPU_UNCACHED_WC:CPU_WRITE_COMBINE GPU_READ_PERMITTED
GPU_WRITE_PERMITTED CPU_READ_PERMITTED CPU_WRITE_PERMITTED"
{
   echo '#include <stdio.h>'
   echo '#include "img_types.h"'
   echo '#include "pvrsrv_memallocflags.h"'
   echo '#include "rgx_memallocflags.h"'
   echo "#define PVR_SRV_DDK $DDK"
   echo '#define BITFIELD_BIT(b) (1ULL << (b))'
   echo "#include \"$WORK/flags.h\""
   echo 'int main(void) { int bad = 0;'
   for pr in $PAIRS; do
      m=${pr%%:*}; k=${pr##*:}
      echo "if ((unsigned long long)(PVR_SRV_MEMALLOCFLAG_$m) != (unsigned long long)(PVRSRV_MEMALLOCFLAG_$k)) {"
      echo "   printf(\"MISMATCH $m mesa=0x%llx kernel=0x%llx\\n\", (unsigned long long)(PVR_SRV_MEMALLOCFLAG_$m), (unsigned long long)(PVRSRV_MEMALLOCFLAG_$k)); bad++; }"
   done
   echo 'if ((unsigned long long)PVR_SRV_MEMALLOCFLAG_DEVICE_FLAG(PM_FW_PROTECT) != (unsigned long long)PVRSRV_MEMALLOCFLAG_DEVICE_FLAG(PMMETA_PROTECT)) { printf("MISMATCH PM_FW_PROTECT\n"); bad++; }'
   echo "printf(\"DDK $DDK: %d mismatches over 26 values\\n\", bad); return bad; }"
} > "$WORK/check.c"
cc -w -m32 -DLINUX -I"$KM/include" -I"$KM/services/include" "$WORK/check.c" -o "$WORK/check"
"$WORK/check"
