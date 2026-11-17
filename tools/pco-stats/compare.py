#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare two pco_stats result files, like shader-db's report.py.

usage: compare.py before.txt after.txt [-s stat,stat,...] [-a]
Per shader the stats that changed (lower is better for all but smp counts, which are shown, not judged),
then totals and helped/hurt counts per stat. -a lists unchanged shaders as well.
"""
import argparse
import sys

STATS = ["igrps", "main", "bitwise", "control", "temps", "spills", "scratch", "loops", "code_size",
         "smp", "smp_pplod", "smp_f16", "smp_raw", "smp_bias", "smp_grad", "smp_replace", "itrsmp", "itr", "vtxins", "ld", "wdf", "shareds"]
JUDGED = {"igrps", "temps", "spills", "scratch", "loops", "code_size"}  # helped/hurt counts


def load(path):
    out = {}
    for line in open(path):
        f = line.split()
        if len(f) < 3:
            continue
        out[(f[0], f[1])] = {k: int(v) for k, v in (kv.split("=") for kv in f[2:])}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("-s", default="igrps,temps,spills,code_size,smp", help="stats to report")
    ap.add_argument("-a", action="store_true", help="list unchanged shaders too")
    a = ap.parse_args()
    stats = [s for s in a.s.split(",") if s]
    unknown = [s for s in stats if s not in STATS]
    if unknown:
        sys.exit("unknown stat(s): " + ", ".join(unknown))
    b, n = load(a.before), load(a.after)
    keys = sorted(set(b) & set(n))
    for side, d, o in (("only in before", b, n), ("only in after", n, b)):
        miss = sorted(set(d) - set(o))
        if miss:
            print(f"{side}: " + ", ".join("/".join(k) for k in miss))
    tot = {s: [0, 0] for s in stats}
    helped = {s: 0 for s in stats}
    hurt = {s: 0 for s in stats}
    changed = 0
    for k in keys:
        parts = []
        for s in stats:
            x, y = b[k].get(s, 0), n[k].get(s, 0)
            tot[s][0] += x
            tot[s][1] += y
            if x != y:
                parts.append(f"{s} {x} -> {y} ({y - x:+d}, {100.0 * (y - x) / x:+.1f}%)" if x else f"{s} {x} -> {y}")
                if s in JUDGED:
                    (helped if y < x else hurt)[s] += 1
        if parts:
            changed += 1
        if parts or a.a:
            print(f"{'/'.join(k):24} " + ("; ".join(parts) if parts else "unchanged"))
    print(f"\n{len(keys)} shaders compared, {changed} changed")
    print(f"{'':10}{'before':>8}{'after':>8}{'delta':>8}{'%':>9}  helped hurt")
    for s in stats:
        x, y = tot[s]
        pct = f"{100.0 * (y - x) / x:+.1f}%" if x else "-"
        hh = f"  {helped[s]:>6} {hurt[s]:>4}" if s in JUDGED else ""
        print(f"{s:10}{x:>8}{y:>8}{y - x:>+8}{pct:>9}{hh}")


if __name__ == "__main__":
    main()
