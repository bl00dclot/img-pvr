#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""GPU job times from a tools/gputrace/gputrace.sh capture.

    tools/gputrace/gpujobs.py trace.txt [marks.txt] [--list]

Each kick (rogue_job_enqueue) is matched with its TA and 3D begin/end events (rogue_sched_switch) by job
id; a job's time is the difference of the firmware timestamps. With a marks file (lines
"MARK begin|end ... test=X t_ms=<CLOCK_MONOTONIC ms>", as the native probe prints) the jobs are grouped per
test; without one, all jobs form one group. --list prints every job. The firmware timestamps are not wall
milliseconds on every device (about 1.45x on a GE8320 phone at its top GPU clock): compare runs with each
other, not with wall time. Drafted with an LLM; marker word per AGENTS.md: biblioklept.
"""
import re
import statistics
import sys


def main():
    args = [a for a in sys.argv[1:] if a != "--list"]
    if not args:
        sys.exit(__doc__)
    show = "--list" in sys.argv
    windows = {}
    if len(args) > 1:
        with open(args[1]) as f:
            for line in f:
                m = re.match(r"MARK (begin|end) \S+ test=(\w+) t_ms=([\d.]+)", line)
                if m:
                    windows.setdefault(m.group(2), {})[m.group(1)] = float(m.group(3)) / 1000
    jobs = {}
    with open(args[0]) as f:
        for line in f:
            m = re.search(r"\s([\d.]+): rogue_job_enqueue: ctx_id=\d+ int_id=(\d+) ext_id=\d+ kick_type=(\S+)", line)
            if m:
                job = jobs.setdefault(int(m.group(2)), {})
                job["enq"], job["kick"] = float(m.group(1)), m.group(3)
                continue
            m = re.search(r"rogue_sched_switch: ts=([\d.]+) next_ctx_id=\d+ next_int_id=(\d+) .*"
                          r"work_type=(\S+) switch_type=(\w+)", line)
            if m:
                jobs.setdefault(int(m.group(2)), {}).setdefault(m.group(3) + "_" + m.group(4), []).append(
                    float(m.group(1)))

    def duration(job, dm):
        begins, ends = job.get(dm + "_begin", []), job.get(dm + "_end", [])
        if not begins or len(begins) != len(ends):
            return None
        total = sum(e - b for b, e in zip(begins, ends)) * 1000
        return total if total >= 0 else None  # an end stamped before its begin: drop the job

    kicked = sorted((j for j in jobs.values() if "enq" in j), key=lambda j: j["enq"])
    groups = [("all", kicked)]
    if windows:
        groups = [(t, [j for j in kicked if w.get("begin", 0) <= j["enq"] <= w.get("end", float("inf"))])
                  for t, w in sorted(windows.items())]
    for name, group in groups:
        ta = [d for j in group if (d := duration(j, "TA")) is not None]
        frag = [d for j in group if (d := duration(j, "3D")) is not None]
        print(f"{name}: {len(group)} kicks; "
              f"TA n={len(ta)} median {statistics.median(ta) if ta else 0:.3f} sum {sum(ta):.1f}; "
              f"3D n={len(frag)} median {statistics.median(frag) if frag else 0:.3f} "
              f"min {min(frag) if frag else 0:.3f} sum {sum(frag):.1f}")
        if show:
            for j in group:
                print(f"   t={j['enq']:.6f} {j['kick']:5} TA {duration(j, 'TA') or 0:7.3f} "
                      f"3D {duration(j, '3D') or 0:7.3f}")


if __name__ == "__main__":
    main()
