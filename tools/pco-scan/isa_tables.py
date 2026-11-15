# Builds matchers for the PCO instruction encodings from src/imagination/pco/pco_isa.py.
# SPDX-License-Identifier: MIT
import os, sys

PCO = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src', 'imagination', 'pco')
sys.path.insert(0, PCO)
import pco_pygen_common as c  # noqa: E402
import pco_isa  # noqa: E402,F401


def _enum_value(cname):
    for e in c.enums.values():
        if e.elems:
            for el in e.elems.values():
                if el.cname == cname:
                    return el.value
    raise KeyError(cname)


def field_get(bs, fname, data):
    """Extract a bit field (pieces are listed MSB first) from bytes."""
    f = bs.fields[fname]
    v = 0
    for p in f.pieces:
        if p.byte >= len(data):
            return None
        v = (v << p.num_bits) | ((data[p.byte] >> p.lo_bit) & ((1 << p.num_bits) - 1))
    return v


class Struct:
    def __init__(self, bs, st):
        self.bs, self.st, self.name, self.size = bs, st, st.bsname, st.num_bytes
        self.setname = bs.bsname
        self.fixed = {}   # field -> required value
        self.free = []    # (struct field name, field name)
        for ef in st.encode_fields:
            fname = ef.name[len(bs.name) + 1:].lower()
            v = ef.value
            if isinstance(v, int):
                self.fixed[fname] = v
            elif v.startswith('s.'):
                self.free.append((v[2:], fname))
            else:
                self.fixed[fname] = _enum_value(v)

    def match(self, data):
        if len(data) < self.size:
            return False
        d = data[:self.size]
        return all(field_get(self.bs, f, d) == v for f, v in self.fixed.items())

    def decode(self, data):
        d = data[:self.size]
        out = {}
        for sf, f in self.free:
            v = field_get(self.bs, f, d)
            ft = self.bs.fields[f].field_type
            if ft.base_type == c.BaseType.enum and ft.enum.elems:
                for n, e in ft.enum.elems.items():
                    if e.value == v:
                        v = n
                        break
            out[sf] = v
        return out


STRUCTS = {sn: [Struct(bs, st) for st in bs.bit_structs.values()] for sn, bs in c.bit_sets.items()}


def candidates(setname, data):
    return [s for s in STRUCTS[setname] if s.match(data)]
