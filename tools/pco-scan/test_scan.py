#!/usr/bin/env python3
# Self-check: decode the PCO-compiled 88-byte fragment shader (texture read, packed store) and
# scan it back out of a padded buffer. Run: python3 -I tools/pco-scan/test_scan.py
# SPDX-License-Identifier: MIT
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pco_scan as s

HEX = (
    '048078020500200057c000f3b0f24080'
    '4000000000ff04816000080000000481'
    '61000800000002806aff57a000f44c84'
    '62800c8880a000ff02806aff5a92069c'
    '208040a04010002c44fff3ffffffffff'
    '348a808744000020'
)


def main():
    raw = bytes.fromhex(HEX.replace(' ', ''))
    assert len(raw) == 88, len(raw)
    names, p = [], 0
    while p < len(raw):
        g = s.decode_group(raw[p:])
        assert g, p
        names += [x.name for _, x, _ in g['ins']]
        p += g['hdr']['len']
        if g['hdr']['end']:
            break
    assert p == 88 and names == ['ditr', 'tst_ext', 'int32_64_ext', 'branch', 'branch', 'wdf', 'smp_brief',
                                 'wdf', 'pck', 'sngl'], names
    hits = s.scan_headers(b'\0' * 6 + raw + b'\0' * 6, 4)
    assert hits and hits[0][0] == 6, hits
    print('ok')


main()
