#!/usr/bin/env python3
"""Package the stripped driver as a driver zip for Android launchers: meta.json,
libvulkan_powervr.so and the third-party notices (NOTICE.txt).

usage: package.py <stripped libvulkan_powervr.so> <armv7|arm64> <output dir>
"""
import json
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = 'libvulkan_powervr.so'


def git(*args):
    try:
        run = lambda *a: subprocess.run(['git', '-C', ROOT, *a], capture_output=True, text=True,
                                        check=True).stdout.strip()
        # Only this repository's history, never an enclosing one.
        if os.path.normcase(os.path.abspath(run('rev-parse', '--show-toplevel'))) != os.path.normcase(ROOT):
            return ''
        return run(*args)
    except (OSError, subprocess.CalledProcessError):
        return ''


def unstripped_sections(path, arch):
    """Names of symbol-table and debug sections left in a little-endian ELF file."""
    data = open(path, 'rb').read()
    want = 1 if arch == 'armv7' else 2
    if data[:4] != b'\x7fELF' or data[4] != want or data[5] != 1:
        sys.exit('error: %s is not a %d-bit little-endian ELF file' % (path, 32 * want))
    u = lambda o, n: int.from_bytes(data[o:o + n], 'little')
    if want == 1:
        shoff, shentsize, shnum, shstrndx = u(0x20, 4), u(0x2e, 2), u(0x30, 2), u(0x32, 2)
        name_at, off_at, off_size = 0, 0x10, 4
    else:
        shoff, shentsize, shnum, shstrndx = u(0x28, 8), u(0x3a, 2), u(0x3c, 2), u(0x3e, 2)
        name_at, off_at, off_size = 0, 0x18, 8
    sections = [shoff + i * shentsize for i in range(shnum)]
    str_off = u(sections[shstrndx] + off_at, off_size)
    names = []
    for s in sections:
        name_off = str_off + u(s + name_at, 4)
        names.append(data[name_off:data.index(b'\0', name_off)].decode('ascii', 'replace'))
    return [n for n in names if n == '.symtab' or n.startswith('.debug')]


def main():
    if len(sys.argv) != 4 or sys.argv[2] not in ('armv7', 'arm64'):
        sys.exit(__doc__)
    lib, arch, out_dir = sys.argv[1:]

    if not os.path.isfile(lib):
        sys.exit('error: %s is missing' % lib)
    leftovers = unstripped_sections(lib, arch)
    if leftovers:
        sys.exit('error: %s is not stripped (%s); run llvm-strip first' % (lib, ', '.join(leftovers[:4])))

    mesa_version = open(os.path.join(ROOT, 'VERSION')).read().strip()
    header = open(os.path.join(ROOT, 'include', 'vulkan', 'vulkan_core.h')).read()
    vk_header = re.search(r'#define VK_HEADER_VERSION (\d+)', header).group(1)
    commit = git('rev-parse', '--short', 'HEAD')
    suffix = '-' + commit if commit else ''
    bits = '32-bit' if arch == 'armv7' else '64-bit'

    meta = {
        'schemaVersion': 1,
        'name': 'PowerVR Rogue (DDK 1.11) (Mesa %s%s)' % (mesa_version, ' ' + commit if commit else ''),
        'description': 'Mesa PowerVR Vulkan for Rogue GPUs on the DDK 1.11 kernel driver, %s.' % bits,
        'author': 'JimVulkan',
        'packageVersion': '1',
        'vendor': 'Mesa',
        'driverVersion': 'Vulkan 1.3.%s' % vk_header,
        'minApi': 29,
        'libraryName': LIB,
    }

    os.makedirs(out_dir, exist_ok=True)
    zip_path = os.path.join(out_dir, 'img-pvr-%s%s-%s.zip' % (mesa_version, suffix, arch))
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('meta.json', json.dumps(meta, indent=2) + '\n')
        z.write(lib, LIB)
        z.write(os.path.join(ROOT, 'android', 'NOTICE.txt'), 'NOTICE.txt')
    print('packaged %s (%.1f MB)' % (zip_path, os.path.getsize(zip_path) / 1e6))


if __name__ == '__main__':
    main()
