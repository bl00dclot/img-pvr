#!/usr/bin/env python3
# Scanner/decoder for PCO instruction-group streams, built from pco_isa.py tables.
# SPDX-License-Identifier: MIT
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import isa_tables as t

PH_ORDER = {  # oporg -> phases present (execution order)
    0: ['p0'], 1: ['p2'], 2: ['be'], 3: ['p0', 'p1'], 4: ['p0', 'p2'],
    5: ['p0', 'p1', 'p2'], 6: ['p0', 'p2', 'be'], 7: ['p0', 'p1', 'p2', 'be'],
}
OPCNT_PH = [('p0', 1), ('p1', 2), ('p2', 4)]


def hdr(d):
    """Returns dict for the igrp header or None when invalid."""
    if len(d) < 2:
        return None
    ext = d[1] >> 7
    h = {'da': d[0] >> 4, 'len': ((d[0] & 15) or 16) * 2, 'ext': ext}
    if not ext:
        h.update(kind='main', size=2, oporg=(d[1] >> 4) & 7, end=False, rpt=1)
    else:
        if len(d) < 3:
            return None
        at = (d[2] >> 5) & 3
        h['size'] = 3
        if at == 0:
            h.update(kind='main', oporg=(d[1] >> 4) & 7, end=bool(d[2] >> 7), rpt=((d[2] >> 1) & 3) + 1)
        elif at == 2:
            oc = (d[1] >> 4) & 7
            h.update(kind='bitwise', opcnt=oc, end=bool(d[2] >> 7), rpt=((d[2] >> 1) & 3) + 1)
        elif at == 3:
            h.update(kind='control', end=False, ctrlop=(d[2] >> 1) & 15, miscctl=d[2] >> 7)
        else:
            return None
    h['olchk'] = bool((d[1] >> 3) & 1)
    h['cc'] = d[1] & 1
    return h


def phases(h):
    if h['kind'] == 'main':
        return PH_ORDER[h['oporg']]
    if h['kind'] == 'bitwise':
        return [p for p, b in OPCNT_PH if h['opcnt'] & b]
    return ['ctrl']


def instr_sizes(h, d):
    """Instruction structs per phase, laid out highest phase first after the header."""
    pos, out = h['size'], []
    for ph in reversed(phases(h)):
        if h['kind'] == 'control':
            setn = 'ctrl'
            ctrlop = h['ctrlop']
            names = {0: 'branch', 1: 'lapc', 2: 'savl', 3: 'cnd', 4: 'wop', 5: 'wdf', 6: 'mutex',
                     7: 'nop', 8: 'itrsmp', 11: 'sbo', 12: 'ditr'}
            nm = names.get(ctrlop)
            if nm is None:
                return None
            cands = [s for s in t.STRUCTS['ctrl'] if s.name == nm or s.name.startswith(nm + '_')]
            cands = [s for s in cands if s.match(d[pos:])]
        else:
            setn = 'backend' if ph == 'be' else ('bitwise' if h['kind'] == 'bitwise' else 'main')
            cands = t.candidates(setn, d[pos:])
            if h['kind'] == 'bitwise':
                cands = [s for s in cands if s.name == f'phase{ph[1]}' or (ph == 'p0' and s.name.startswith('phase0'))]
        if not cands:
            return None
        s = max(cands, key=lambda s: s.size)
        out.append((ph, s, s.decode(d[pos:])))
        pos += s.size
    return out, pos


def decode_group(d):
    h = hdr(d)
    if h is None or h['len'] > len(d):
        return None
    g = d[:h['len']]
    r = instr_sizes(h, g)
    if r is None:
        return None
    ins, pos = r
    return {'hdr': h, 'ins': ins, 'io_off': pos, 'bytes': g}


def scan_chain(buf, off):
    """Decode groups from off until an end group; returns list or None."""
    groups, p = [], off
    while p < len(buf):
        if buf[p] == 0 or buf[p] == 0xff or (buf[p] >> 4) == 0xf:   # zero/padding
            return None
        g = decode_group(buf[p:])
        if g is None:
            return None
        g['off'] = p
        groups.append(g)
        p += g['hdr']['len']
        if g['hdr']['end']:
            return groups
    return None


def scan_headers(buf, min_groups=4):
    """Header-only chain finder: groups whose lengths chain up to an end group.
    Returns [(offset, [(hdr, off)])] for chains that are not a suffix of an earlier hit."""
    hits, covered = [], set()
    for o in range(len(buf) - 2):
        if o in covered:
            continue
        p, ch = o, []
        while p + 3 <= len(buf) and buf[p] not in (0, 0xff) and (buf[p] >> 4) != 0xf:
            h = hdr(buf[p:])
            if h is None or p + h['len'] > len(buf) or h['len'] < h['size']:
                ch = []
                break
            ch.append((h, p))
            p += h['len']
            if h['end']:
                break
        else:
            ch = []
        if len(ch) >= min_groups and ch[-1][0]['end']:
            hits.append((o, ch))
            covered.update(g[1] for g in ch)
    return hits


def report(path):
    import re
    buf = open(path, 'rb').read()
    be32 = lambda o: int.from_bytes(buf[o:o + 4], 'big')
    print(f'{path}: {len(buf)} bytes; BE32@0x0c={be32(0xc)} BE32@0x28={be32(0x28)} word@4={buf[4:8].hex()}')
    print('  strings:', [x.decode() for x in re.findall(rb'[A-Za-z_0-9.]{3,}', buf)])
    for o, ch in scan_headers(buf):
        n = sum(g[0]['len'] for g in ch)
        print(f'  code chain @0x{o:x}: {len(ch)} groups, {n} bytes')
        for h, p in ch:
            g = decode_group(buf[p:])
            desc = ' '.join(f'{ph}:{x.name}' for ph, x, _ in g['ins']) if g else 'UNDECODED'
            print(f'    +{p - o:4d} {h["kind"]:8s} len {h["len"]:2d} da {h["da"]} {desc}')


if __name__ == '__main__':
    for f in sys.argv[1:]:
        report(f)
