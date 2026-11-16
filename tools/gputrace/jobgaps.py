#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Per-job split of a tools/gputrace/gputrace.sh capture: TA time, TA-end to 3D-begin, 3D time, 3D-end to the next
3D-begin, and the period between 3D begins. Firmware milliseconds (valid inside one GED level only).

    jobgaps.py trace.txt [--last N] [--wall-ms W] [--list]

--last N keeps the last N TA3D kicks (the timed block of vkbench/glwork bench). --wall-ms W (wall ms per pass
printed by the bench) prints the wall / firmware conversion ratio. Medians over the kept jobs; first and last job
of the window are dropped from the gap columns. Drafted with an LLM; marker word per AGENTS.md: biblioklept.
"""
import re
import statistics as st
import sys


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    last = int(a[a.index("--last") + 1]) if "--last" in a else 0
    wall = float(a[a.index("--wall-ms") + 1]) if "--wall-ms" in a else 0
    enq, ev = [], {}
    for line in open(a[0]):
        m = re.search(r"\s([\d.]+): rogue_job_enqueue: .* int_id=(\d+) .*kick_type=(\S+)", line)
        if m:
            enq.append((float(m[1]), int(m[2]), m[3]))
            continue
        m = re.search(r"\s([\d.]+): rogue_sched_switch: ts=([\d.]+) .*next_int_id=(\d+) .*work_type=(\S+) switch_type=(\w+)", line)
        if m:
            ev.setdefault(int(m[3]), {}).setdefault((m[4], m[5]), []).append((float(m[2]), float(m[1])))
    ms = lambda x: x * 1000
    jobs = []
    for h, i, k in sorted(enq):
        e = ev.get(i, {})
        if k != "TA3D" or not all(len(e.get(x, [])) == 1 for x in (("TA", "begin"), ("TA", "end"), ("3D", "begin"), ("3D", "end"))):
            continue
        jobs.append((h, e[("TA", "begin")][0][0], e[("TA", "end")][0][0], e[("3D", "begin")][0][0],
                     e[("3D", "end")][0][0], e[("3D", "end")][0][1]))
    if last:
        jobs = jobs[-last:]
    if "--list" in a:
        for j in jobs:
            print(f"  TA {ms(j[2] - j[1]):.4f}  TA end->3D {ms(j[3] - j[2]):.4f}  3D {ms(j[4] - j[3]):.4f}  (fw ms)")
    n = len(jobs)
    if n < 4:
        sys.exit("too few complete TA3D jobs: %d" % n)
    rows = []
    for j, nx in zip(jobs, jobs[1:]):
        rows.append((ms(j[2] - j[1]), ms(j[3] - j[2]), ms(j[4] - j[3]), ms(nx[3] - j[4]), ms(nx[3] - j[3]),
                     ms(nx[1] - j[1]), ms(nx[1] - j[2])))
    names = ["TA", "TA end->3D begin", "3D", "3D end->next 3D begin", "period 3D->3D", "period TA->TA",
             "TA end->next TA begin"]
    print(f"{n} TA3D jobs; medians (min..max) fw ms:")
    for c, nm in enumerate(names):
        col = [r[c] for r in rows]
        print(f"  {nm:24s} {st.median(col):8.4f}  ({min(col):.4f}..{max(col):.4f})")
    span_fw = ms(jobs[-1][4] - jobs[0][3])
    span_host = ms(jobs[-1][5] - jobs[0][5])
    print(f"  fw span first 3D begin -> last 3D end {span_fw:.2f} ms, per job {span_fw / n:.4f}")
    if wall:
        print(f"  wall/fw ratio: wall {wall:.4f} ms/pass vs fw mean period {span_fw / (n - 1):.4f} -> {wall / (span_fw / (n - 1)):.3f}")


if __name__ == "__main__":
    main()
