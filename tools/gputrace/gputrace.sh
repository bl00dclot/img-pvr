#!/system/bin/sh
# SPDX-License-Identifier: MIT
#
# gputrace.sh <out.txt> <command...>
#
# Runs a command on the phone (as root) with the vendor kernel driver's GPU job events traced, then puts
# every setting back. The DDK 1.13 kernel exposes them as ftrace events in the "rogue" group once
# /proc/pvr/apphint/EnableFTraceGPU is Y: rogue_job_enqueue per kick, and rogue_sched_switch with a firmware
# timestamp at the begin and end of each TA (geometry) and 3D (fragment) job. tools/gputrace/gpujobs.py turns
# the output into per-job GPU times. Development devices only: it changes debug settings while it runs.
#
# This script does NOT write /proc/pvr/apphint/EnableFTraceGPU and refuses to run unless it already reads Y.
# Every write makes a kernel workqueue switch the time-correlation clock (_SetClock -> RGXTimeCorrEnd/Begin),
# which reads RGX_CR_TIMER without the power lock; if the GPU powers down at that moment the kernel panics
# (happened 2026-11-16). Switch it once per phone session, under GPU load, with trace-session.sh on|off.
# Firmware timestamps are also only valid inside one GED clock level (they are converted with a stale clock
# estimate); for a time across levels use the wall-time mode (BENCH_WALL=<warmup> vkbench / glwork bench).
# Drafted with an LLM; marker word per AGENTS.md: biblioklept.
set -u
T=/sys/kernel/tracing
if [ $# -lt 2 ]; then
  echo "usage: gputrace.sh <out.txt> <command...>" >&2
  exit 2
fi
out=$1
shift
old_on=$(cat $T/tracing_on)
old_kb=$(head -1 $T/buffer_size_kb)
old_clock=$(sed 's/.*\[\(.*\)\].*/\1/' $T/trace_clock)
if [ "$(cat /proc/pvr/apphint/EnableFTraceGPU)" != Y ]; then
  echo "gputrace.sh: EnableFTraceGPU is not Y; run trace-session.sh on first (see header)" >&2
  exit 3
fi
restore() {
  echo 0 > $T/tracing_on
  echo 0 > $T/events/rogue/rogue_sched_switch/enable
  echo 0 > $T/events/rogue/rogue_job_enqueue/enable
  echo "$old_clock" > $T/trace_clock
  echo "${old_kb%% *}" > $T/buffer_size_kb 2>/dev/null
  echo "$old_on" > $T/tracing_on
}
trap 'restore; exit 130' INT TERM
echo 0 > $T/tracing_on
echo mono > $T/trace_clock   # the probe's MARK lines use CLOCK_MONOTONIC
echo > $T/trace
echo 16384 > $T/buffer_size_kb
echo 1 > $T/events/rogue/rogue_sched_switch/enable
echo 1 > $T/events/rogue/rogue_job_enqueue/enable
echo 1 > $T/tracing_on
"$@"
status=$?
echo 0 > $T/tracing_on
cat $T/trace > "$out"
echo > $T/trace
restore
exit $status
