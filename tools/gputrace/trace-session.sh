#!/system/bin/sh
# SPDX-License-Identifier: MIT
#
# XCLIPSE: trace-session.sh on|off      (root, once per phone session; then gputrace.sh any number of times)
#
# Switches /proc/pvr/apphint/EnableFTraceGPU to Y (on) or N (off), only while a GPU load is running.
# Why: a kernel panic followed one apphint write (2026-11-16). Suspected cause: the write makes a kernel
# workqueue call _SetClock -> RGXTimeCorrEnd/RGXTimeCorrBegin, which read RGX_CR_TIMER without the power lock
# (read in another device's DDK 1.13 kernel source, not checked against this phone's kernel). The load during
# the switch is a precaution, not proven. The switch is done once per session, never per run. Refuses if the
# value already is the one asked. Development devices only.
# Load: LOAD="<command>" (default: glwork bench tv, vendor GLES, from /data/local/tmp/s0).
# Drafted with an LLM; marker word per AGENTS.md: biblioklept.
set -u
[ "$(id -u)" = 0 ] || { echo "trace-session.sh: needs root" >&2; exit 2; }
A=/proc/pvr/apphint/EnableFTraceGPU
case "${1:-}" in on) want=Y ;; off) want=N ;; *) echo "usage: trace-session.sh on|off" >&2; exit 2 ;; esac
cur=$(cat $A)
if [ "$cur" = "$want" ]; then
  echo "trace-session.sh: EnableFTraceGPU already $cur; not writing" >&2
  exit 3
fi
${LOAD:-/data/local/tmp/s0/glwork bench tv 100000} >/dev/null 2>&1 &
load=$!
trap 'kill $load 2>/dev/null' EXIT INT TERM
sleep 1
kill -0 $load 2>/dev/null || { echo "trace-session.sh: load did not start; not writing" >&2; exit 4; }
echo $want > $A
i=0
while [ "$(cat $A)" != "$want" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
sleep 1   # let the workqueue finish while the load still runs
kill $load 2>/dev/null
wait $load 2>/dev/null
[ "$(cat $A)" = "$want" ] || { echo "trace-session.sh: read back $(cat $A), wanted $want; the apphint state is unknown, check it with: cat $A" >&2; exit 5; }
echo "EnableFTraceGPU=$want"
