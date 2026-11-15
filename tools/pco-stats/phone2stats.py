#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turn the device log of `PCO_DEBUG_PRINT=vs,fs,stats PCO_COLOR=off` (one pipeline) into pco_stats result lines.

usage: phone2stats.py name < log > lines.txt      (several logs: cat the outputs of one name each)
The stats come from the driver's `VS shader: ...` / `FS shader: ...` lines; the sample counts are taken from the
last `shader ir after passes:` block before each of them, with the same token rules as pco_stats.c.
"""
import re
import sys

KEYS = {"code size": "code_size", "scratch size": "scratch", "spill count": "spills", "temp count": "temps",
        "vtxin count": "vtxins", "loop count": "loops", "inst group count": "igrps",
        "main inst group count": "main", "bitwise inst group count": "bitwise",
        "control inst group count": "control"}


def counts(ir):
    c = dict.fromkeys(["smp", "smp_pplod", "smp_f16", "smp_raw", "smp_bias", "smp_grad", "smp_replace", "itrsmp", "itr"], 0)
    for t in re.findall(r"(?<![A-Za-z0-9_.])[A-Za-z0-9_.]+", ir):
        if t == "smp" or t.startswith("smp."):
            m = t.split(".")[1:] + [""]
            c["smp"] += 1
            c["smp_pplod"] += "pplod" in m[:-1]
            c["smp_f16"] += "f16" in m[:-1]
            c["smp_raw"] += "rawdata" in m[:-1] or "both" in m[:-1]
            c["smp_bias"] += "bias" in m[:-1]
            c["smp_grad"] += "gradients" in m[:-1]
            c["smp_replace"] += "replace" in m[:-1]
        elif t.startswith("itrsmp"):
            c["itrsmp"] += 1
        elif t.startswith("fitr") or t.startswith("ditr"):
            c["itr"] += 1
    return c


def main():
    name = sys.argv[1]
    ir, in_ir = [], False
    for line in sys.stdin:
        if line.startswith("shader ir"):
            in_ir = line.startswith("shader ir after passes")
            ir = [] if in_ir else ir
            continue
        m = re.match(r"(VS|FS) shader: (.*)", line)
        if m:
            st = {KEYS[k]: int(v) for v, k in (re.match(r"(\d+) (.*)", p.strip()).groups() for p in m.group(2).split(","))}
            st.update(counts("".join(ir)))
            order = ["code_size", "igrps", "main", "bitwise", "control", "temps", "vtxins", "spills", "scratch", "loops",
                     "smp", "smp_pplod", "smp_f16", "smp_raw", "smp_bias", "smp_grad", "smp_replace", "itrsmp", "itr"]
            print(name, m.group(1).lower(), " ".join(f"{k}={st[k]}" for k in order))
            in_ir = False
        elif in_ir:
            ir.append(line)


if __name__ == "__main__":
    main()
