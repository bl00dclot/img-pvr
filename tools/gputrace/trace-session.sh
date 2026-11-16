#!/system/bin/sh
# SPDX-License-Identifier: MIT
#
# trace-session.sh on|off      (root, once per phone session; then gputrace.sh any number of times)
#
# Switches /proc/pvr/apphint/EnableFTraceGPU to Y (on) or N (off), only while a GPU load is running.
# Why: the write makes a kernel workqueue call _SetClock -> RGXTimeCorrEnd/RGXTimeCorrBegin, which read
# RGX_CR_TIMER without the power lock; if the GPU powers down between or at those reads the kernel panics
# (2026-11-16). With a load running the GPU stays powered. Steady state with the trace on is safe, only
# the transition is risky, so it is done once, never per run. Refuses if the value already is the one asked.
# Load: LOAD="<command>" (default: glwork bench tv, vendor GLES, from /data/local/tmp/s0).
# Drafted with an LLM; marker word per AGENTS.md: biblioklept.
set -u
A=/proc/pvr/apphint/EnableFTraceGPU
case "${1:-}" in on) want=Y ;; off) want=N ;; *) echo "usage: trace-session.sh on|off" >&2; exit 2 ;; esac
cur=$(cat $A)
if [ "$cur" = "$want" ]; then
  echo "trace-session.sh: EnableFTraceGPU already $cur; not writing" >&2
  exit 3
fi
${LOAD:-/data/local/tmp/s0/glwork bench tv 100000} >/dev/null 2>&1 &
load=$!
sleep 1
kill -0 $load 2>/dev/null || { echo "trace-session.sh: load did not start; not writing" >&2; exit 4; }
echo $want > $A
i=0
while [ "$(cat $A)" != "$want" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
sleep 1   # let the workqueue finish while the load still runs
kill $load 2>/dev/null
wait $load 2>/dev/null
[ "$(cat $A)" = "$want" ] || { echo "trace-session.sh: read back $(cat $A), wanted $want" >&2; exit 5; }
echo "EnableFTraceGPU=$want"
