#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Table of 3D job times from a tools/gputrace/bench.sh output directory.

    tools/gputrace/bench_table.py <dir>

For each mode: median 3D job time of the Vulkan driver and of the vendor GLES driver (firmware time units,
see gpujobs.py), their ratio, and each run's printed result line. Only jobs of the passes are counted: the
3D jobs whose time is within 4x of the median 3D job of the run, which drops set-up and readback jobs.
Drafted with an LLM; marker word per AGENTS.md: biblioklept.
"""
import os
import re
import statistics
import sys


def frag_times(path):
    stamps = {}
    with open(path) as f:
        for line in f:
            m = re.search(r"rogue_sched_switch: ts=([\d.]+) next_ctx_id=\d+ next_int_id=(\d+) .*"
                          r"work_type=3D switch_type=(\w+)", line)
            if m:
                stamps.setdefault(int(m.group(2)), {}).setdefault(m.group(3), []).append(float(m.group(1)))
    times = []
    for job in stamps.values():
        begins, ends = job.get("begin", []), job.get("end", [])
        if begins and len(begins) == len(ends):
            t = sum(e - b for b, e in zip(begins, ends)) * 1000
            if t > 0:
                times.append(t)
    if not times:
        return []
    med = statistics.median(times)
    return [t for t in times if med / 4 <= t <= med * 4]


def result_line(path):
    try:
        with open(path) as f:
            lines = [l.strip() for l in f if "bench" in l and "sum" in l]
        return lines[-1] if lines else "(no result line)"
    except OSError:
        return "(missing)"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    modes = sorted({m.group(2) for f in os.listdir(d) if (m := re.match(r"(vk|gl)-(\w+)\.trace$", f))})
    print(f"{'mode':8} {'vk 3D':>8} {'gl 3D':>8} {'vk/gl':>6}  n(vk/gl)")
    for mode in modes:
        vk = frag_times(os.path.join(d, f"vk-{mode}.trace"))
        gl = frag_times(os.path.join(d, f"gl-{mode}.trace"))
        v = statistics.median(vk) if vk else float("nan")
        g = statistics.median(gl) if gl else float("nan")
        print(f"{mode:8} {v:8.3f} {g:8.3f} {v / g if g else float('nan'):6.2f}  {len(vk)}/{len(gl)}")
    print()
    for mode in modes:
        print(f"{mode}: vk {result_line(os.path.join(d, f'vk-{mode}.txt'))}")
        print(f"{' ' * len(mode)}  gl {result_line(os.path.join(d, f'gl-{mode}.txt'))}")
    for name in ("hwr-before.txt", "hwr-after.txt"):
        p = os.path.join(d, name)
        if os.path.exists(p):
            print(name, open(p).read().strip())


if __name__ == "__main__":
    main()
