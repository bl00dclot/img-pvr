#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Makes forged VkPipelineCache files from a real one (saved by `VKBENCH_CACHE=<file> vkbench ...`) for
# tools/vktest/pvr_cache_fuzz.sh. The cache objects of the PowerVR driver end in a CRC32 that is no
# authentication, so every mutant has the CRC of its object recomputed: the driver has to refuse the data on
# its content. Mutants: random byte and dword (bound edge) replacements in the payload, every flag
# combination, truncations and extensions, payloads of one pattern, lying container fields.
#
#   pvr_cache_fuzz_gen.py <real cache file> <output dir> [seed]
#
# Writes <output dir>/NNNN-<kind>.bin and index.txt (name, kind, object index, whether the object should
# certainly not be accepted: a kind of change the driver must refuse).
#
# Drafted with an LLM; marker word per AGENTS.md: biblioklept.

import random
import struct
import sys
import zlib

HEADER = 32
ALIGN = 8
CRC_MAGIC = 0x32435650


def crc(data):
    # util_hash_crc32 with zlib: ~crc32(0, data)
    return (~zlib.crc32(data)) & 0xFFFFFFFF


def parse(blob):
    count = struct.unpack_from('<I', blob, HEADER)[0]
    off = HEADER + 4
    objs = []
    for _ in range(count):
        typ, ksz, dsz = struct.unpack_from('<III', blob, off)
        off += 12
        key = blob[off:off + ksz]
        off += ksz
        off = (off + ALIGN - 1) & ~(ALIGN - 1)
        data = blob[off:off + dsz]
        off += dsz
        objs.append([typ, key, bytearray(data)])
    assert off == len(blob), (off, len(blob))
    return blob[:HEADER], objs


def build(header, objs, count=None, lie=None):
    out = bytearray(header)
    out += struct.pack('<I', len(objs) if count is None else count)
    for i, (typ, key, data) in enumerate(objs):
        dsz = len(data) if lie is None or lie[0] != i else lie[1]
        out += struct.pack('<III', typ, len(key), dsz)
        out += key
        while len(out) % ALIGN:
            out.append(0)
        out += data
    return bytes(out)


def fix(data):
    data[-4:] = struct.pack('<I', crc(bytes(data[:-4])))
    return data


def main():
    src, outdir, seed = sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rnd = random.Random(seed)
    header, objs = parse(open(src, 'rb').read())
    assert all(struct.unpack_from('<I', o[2], 0)[0] == CRC_MAGIC for o in objs), 'not a PVC2 cache'
    index = []
    n = [0]

    def emit(kind, which, mutated, must_fail=False, **kw):
        o = [list(x) for x in objs]
        if mutated is not None:
            o[which][2] = fix(bytearray(mutated))
        name = '%04d-%s.bin' % (n[0], kind)
        n[0] += 1
        open('%s/%s' % (outdir, name), 'wb').write(build(header, o, **kw))
        index.append('%s %s %d %d' % (name, kind, which, int(must_fail)))

    edge = [0, 1, 2, 3, 4, 7, 8, 0xFF, 0x100, 0xFFF, 0x1000, 0x1001, 0xFFFF, 0x10000, 0x10001,
            0x7FFFFFFF, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF]
    sizes = sorted(set(len(o[2]) for o in objs))
    targets = [rnd.randrange(len(objs)) for _ in range(8)]
    # also the biggest and the smallest object
    targets += [max(range(len(objs)), key=lambda i: len(objs[i][2])),
                min(range(len(objs)), key=lambda i: len(objs[i][2]))]

    for _ in range(160):
        w = rnd.choice(targets)
        d = bytearray(objs[w][2])
        pos = rnd.randrange(8, len(d) - 4)
        d[pos] = rnd.choice([0, 1, 0x7F, 0x80, 0xFF, rnd.randrange(256)])
        emit('byte', w, d)

    for _ in range(200):
        w = rnd.choice(targets)
        d = bytearray(objs[w][2])
        pos = rnd.randrange(8, len(d) - 8) & ~3
        struct.pack_into('<I', d, pos, rnd.choice(edge))
        emit('dword', w, d)

    # Pointer-like values (what the old raw-struct format trusted): 8 bytes of 0x41 or a kernel address
    # at random dword positions.
    for _ in range(40):
        w = rnd.choice(targets)
        d = bytearray(objs[w][2])
        pos = rnd.randrange(8, len(d) - 12) & ~3
        d[pos:pos + 8] = rnd.choice([b'\x41' * 8, struct.pack('<Q', 0xFFFFFFC000000000),
                                     struct.pack('<II', 0xDEADBEEF, 0xDEADBEEF)])
        emit('pointer', w, d)

    # Flags: every combination of the five defined bits and some undefined ones.
    for w in targets[:3]:
        for fl in list(range(0, 64)) + [0x100, 0x80000000, 0xFFFFFFFF]:
            d = bytearray(objs[w][2])
            struct.pack_into('<I', d, 4, fl)
            orig = struct.unpack_from('<I', objs[w][2], 4)[0]
            emit('flags', w, d, must_fail=fl != orig)

    # Not a VS (stage 0 parsed as a CS) / no FS (stage 1 flag cleared): both must be refused.
    for w in targets[:4]:
        orig = struct.unpack_from('<I', objs[w][2], 4)[0]
        for name, fl in (('noIsVS', orig & ~4), ('noFS', orig & ~2), ('plusXFB', orig | 16)):
            d = bytearray(objs[w][2])
            struct.pack_into('<I', d, 4, fl)
            emit(name, w, d, must_fail=fl != orig)

    for _ in range(40):
        w = rnd.choice(targets)
        n_ = rnd.randrange(4, len(objs[w][2]) - 4)
        d = bytearray(objs[w][2][:n_] + b'\0\0\0\0')
        emit('trunc', w, d, must_fail=True)

    for w in targets[:3]:
        d = bytearray(objs[w][2][:-4] + bytes([0x41]) * 64 + b'\0\0\0\0')
        emit('extend', w, d, must_fail=True)
        for pat in (0x41, 0xFF, 0x00):
            d = bytearray(objs[w][2][:8] + bytes([pat]) * (len(objs[w][2]) - 12) + b'\0\0\0\0')
            emit('pattern%02x' % pat, w, d, must_fail=True)

    # CRC only: payload untouched but CRC wrong
    for w in targets[:3]:
        d = bytearray(objs[w][2])
        d[-1] ^= 0xFF
        o = [list(x) for x in objs]
        o[w][2] = d
        name = '%04d-badcrc.bin' % n[0]
        n[0] += 1
        open('%s/%s' % (outdir, name), 'wb').write(build(header, o))
        index.append('%s badcrc %d 1' % (name, w))

    # Lying container fields (the common runtime parses these): object type, key size, data size
    for w in targets[:3]:
        for typ in (1, 2, 5, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x1F000000):
            o = [list(x) for x in objs]
            o[w][0] = typ
            name = '%04d-type.bin' % n[0]
            n[0] += 1
            open('%s/%s' % (outdir, name), 'wb').write(build(header, o))
            index.append('%s type %d 0' % (name, w))
        for ksz in (0, 1, 31, 33, 4096):
            o = [list(x) for x in objs]
            o[w][1] = (o[w][1] + bytes(4096))[:ksz]
            name = '%04d-keysize.bin' % n[0]
            n[0] += 1
            open('%s/%s' % (outdir, name), 'wb').write(build(header, o))
            index.append('%s keysize %d 0' % (name, w))
        for dsz in (0, 1, 3, 4, 8):
            emit('dsize-small', w, None, lie=(w, dsz))
    for w in range(len(objs)):
        emit('dsize-low', w, None, lie=(w, len(objs[w][2]) - 7))
    w = targets[0]
    emit('count-high', w, None, count=len(objs) + 5)
    emit('count-huge', w, None, count=0xFFFFFFFF)
    emit('dsize-high', w, None, lie=(w, len(objs[w][2]) + 1000))
    emit('dsize-huge', w, None, lie=(w, 0xFFFFFFF0))

    open('%s/index.txt' % outdir, 'w').write('\n'.join(index) + '\n')
    print('%d files, %d objects in the source' % (n[0], len(objs)))


main()
