#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarise a pvrtrace capture: one line per bridge call, and the kick command blobs as u32 words.

    tools/pvrtrace/decode.py capture.bin [--words] [--func KICKTA3D2]

Names come from the generated pvr_srv_bridge_ddk113.h (DDK113_BRIDGE_<GROUP>_<FUNC> defines).
"""
import argparse
import os
import re
import struct
import sys

GROUPS = {1: "SRVCORE", 2: "SYNC", 6: "MM", 11: "DMABUF", 13: "CACHE", 128: "RGXTQ", 129: "RGXCMP",
          130: "RGXTA3D", 134: "RGXHWPERF"}
TAGS = {1: "TA", 2: "3D-PR", 3: "3D", 4: "CDM"}


def func_names():
    hdr = os.path.join(os.path.dirname(__file__), "../../src/imagination/vulkan/winsys/pvrsrvkm/"
                       "pvr_srv_bridge_ddk113.h")
    names = {}
    with open(hdr) as fh:
        text = fh.read()
    for g, f, n in re.findall(r"#define DDK113_BRIDGE_([A-Z0-9]+)_([A-Z0-9_]+) (\d+)U", text):
        names[(g, int(n))] = f
    return names


def records(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"PVRT" or len(data) < 8:
        sys.exit("not a pvrtrace capture")
    if struct.unpack_from("<I", data, 4)[0] != 2:
        sys.exit("unsupported pvrtrace capture version (expected 2)")

    def take(off, size):
        if size < 0 or off + size > len(data):
            sys.exit(f"capture truncated at offset {off}")
        return data[off:off + size], off + size

    off = 8
    while off < len(data):
        hdr, off = take(off, 36)
        magic, flags, tid, group, func, ret, insz, outsz, nblobs = struct.unpack("<9I", hdr)
        if magic != 0x52454332:
            sys.exit(f"bad record at offset {off - 36}")
        inb, off = take(off, insz)
        outb, off = take(off, outsz)
        blobs = []
        for _ in range(nblobs):
            bh, off = take(off, 8)
            tag, size = struct.unpack("<2I", bh)
            b, off = take(off, size)
            blobs.append((tag, b))
        yield tid, group, func, struct.unpack("<i", struct.pack("<I", ret))[0], inb, outb, blobs, flags


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--words", action="store_true", help="print kick blobs as u32 words")
    ap.add_argument("--func", help="only calls whose function name contains this")
    a = ap.parse_args()
    names = func_names()
    for i, (tid, g, f, ret, inb, outb, blobs, flags) in enumerate(records(a.capture)):
        gname = GROUPS.get(g, str(g))
        fname = names.get((gname, f), str(f))
        if a.func and a.func not in fname:
            continue
        err = struct.unpack_from("<I", outb, len(outb) - 4)[0] if len(outb) >= 4 and not flags & 1 else "unread"
        print(f"#{i} tid={tid} {gname}.{fname} in={len(inb)} out={len(outb)} ioctl={ret} eError={err}"
              + "".join(f" [{TAGS.get(t, hex(t))}:{len(b)}B]" for t, b in blobs)
            + (" [PREPARES TRUNCATED]" if flags & 2 else "")
            + (" [BLOB MISSING]" if flags & 4 else "")
            + (" [EDITED]" if flags & 8 else "")
            + (" [EDIT REFUSED]" if flags & 16 else ""))
        if a.words:
            for t, b in blobs:
                words = struct.unpack(f"<{len(b) // 4}I", b[:len(b) // 4 * 4])
                for j in range(0, len(words), 8):
                    print(f"    {TAGS.get(t, hex(t))} +0x{j * 4:03x}: " +
                          " ".join(f"{w:08x}" for w in words[j:j + 8]))


if __name__ == "__main__":
    main()
