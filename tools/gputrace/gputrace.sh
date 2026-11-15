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
old_ft=$(cat /proc/pvr/apphint/EnableFTraceGPU)
echo 0 > $T/tracing_on
echo mono > $T/trace_clock   # the probe's MARK lines use CLOCK_MONOTONIC
echo > $T/trace
echo 16384 > $T/buffer_size_kb
echo Y > /proc/pvr/apphint/EnableFTraceGPU   # before the events: enabling them first records nothing
echo 1 > $T/events/rogue/rogue_sched_switch/enable
echo 1 > $T/events/rogue/rogue_job_enqueue/enable
echo 1 > $T/tracing_on
"$@"
status=$?
echo 0 > $T/tracing_on
echo 0 > $T/events/rogue/rogue_sched_switch/enable
echo 0 > $T/events/rogue/rogue_job_enqueue/enable
echo "$old_ft" > /proc/pvr/apphint/EnableFTraceGPU
cat $T/trace > "$out"
echo > $T/trace
echo "$old_clock" > $T/trace_clock
echo "$old_kb" > $T/buffer_size_kb 2>/dev/null
echo "$old_on" > $T/tracing_on
exit $status
