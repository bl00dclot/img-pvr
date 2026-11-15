#!/usr/bin/env python3
"""Write an SPDX 2.3 JSON SBOM for a driver build from the values the build pins.

usage: gen-sbom.py --ndk <ndk dir> --packages <dpkg-query list> --zip <driver zip>... > sbom.spdx.json

  --ndk       the NDK used (its source.properties gives the revision; the archive SHA-256 is read from
              .github/workflows/ddk113-release.yml)
  --packages  "name version" lines from the build image: docker run --rm img-pvr-build:trixie
              dpkg-query -W -f '${Package} ${Version}\\n'   (the apt packages, LLVM/clang included)
  --zip       the release archive(s): they are the SBOM's described packages
Pinned inputs read from the tree: docker/Dockerfile (base image digest, Debian snapshot date),
subprojects/{libdrm,expat,zlib}.wrap (versions and hashes), VERSION and the import commit (Mesa base).
Output is deterministic for the same inputs (timestamp from SOURCE_DATE_EPOCH).
"""
import argparse, configparser, hashlib, json, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rd = lambda p: open(os.path.join(ROOT, p)).read()
git = lambda *a: subprocess.run(['git', '-C', ROOT, *a], capture_output=True, text=True, check=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ndk', required=True)
    ap.add_argument('--packages', required=True)
    ap.add_argument('--zip', action='append', required=True)
    a = ap.parse_args()

    docker = rd('docker/Dockerfile')
    digest = re.search(r'^FROM debian@sha256:([0-9a-f]{64})', docker, re.M).group(1)
    snapshot = re.search(r'^ARG SNAPSHOT=(\S+)', docker, re.M).group(1)
    wf = rd('.github/workflows/ddk113-release.yml')
    ndk_sha = re.search(r'echo "([0-9a-f]{64})  \S+ndk\.zip"', wf).group(1)
    ndk_url = re.search(r'https://dl\.google\.com/\S+android-ndk-\S+\.zip', wf).group(0)
    props = dict(l.split(' = ', 1) for l in open(os.path.join(a.ndk, 'source.properties')).read().splitlines() if ' = ' in l)
    base = git('rev-list', '--max-parents=0', 'HEAD').splitlines()[-1]
    head = git('rev-parse', 'HEAD')
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH') or git('log', '-1', '--format=%ct'))
    n = 0

    def pkg(name, version, download='NOASSERTION', sha256=None, comment=None, purl=None, spdxid=None):
        nonlocal n
        n += 1
        p = {'SPDXID': spdxid or 'SPDXRef-P%d' % n, 'name': name, 'versionInfo': version, 'downloadLocation': download,
             'filesAnalyzed': False, 'licenseConcluded': 'NOASSERTION', 'licenseDeclared': 'NOASSERTION',
             'copyrightText': 'NOASSERTION'}
        if sha256: p['checksums'] = [{'algorithm': 'SHA256', 'checksumValue': sha256}]
        if comment: p['comment'] = comment
        if purl: p['externalRefs'] = [{'referenceCategory': 'PACKAGE-MANAGER', 'referenceType': 'purl', 'referenceLocator': purl}]
        return p

    described, deps = [], []
    for z in a.zip:
        h = hashlib.sha256(open(z, 'rb').read()).hexdigest()
        described.append(pkg(os.path.basename(z), open(os.path.join(ROOT, 'VERSION')).read().strip(), sha256=h,
                             comment='Release archive; built from commit %s' % head, spdxid='SPDXRef-Release%d' % (len(described) + 1)))
    deps.append(pkg('mesa-powervr-base', open(os.path.join(ROOT, 'VERSION')).read().strip(),
                    'git+https://github.com/JimVulkan/img-pvr@' + base,
                    comment='Base of this fork: the img-pvr import commit %s ("Based on Mesa 27.0.0-devel"). The upstream '
                            'Mesa commit it was taken from is not recorded in the tree.' % base))
    deps.append(pkg('android-ndk', props['Pkg.Revision'], ndk_url, ndk_sha,
                    'NDK %s; its bundled LLVM is the cross compiler (AndroidVersion.txt of the toolchain names the clang revision).' % props['Pkg.ReleaseName']))
    deps.append(pkg('build-image-debian', 'trixie', 'docker://debian@sha256:' + digest, digest,
                    'Base image of docker/Dockerfile (digest of the multi-arch image index); apt packages come from snapshot.debian.org/archive/debian/%s.' % snapshot))
    for w in ('libdrm', 'expat', 'zlib'):
        c = configparser.ConfigParser()
        c.read(os.path.join(ROOT, 'subprojects', w + '.wrap'))
        s = c['wrap-file']
        deps.append(pkg(w, re.sub(r'^\D+-', '', s['directory']), s['source_url'], s['source_hash'],
                        'Meson wrap subproject, linked into the driver' if w != 'expat' else 'Meson wrap subproject (configure step; not linked into the driver)'))
    for line in sorted(open(a.packages).read().splitlines()):
        name, ver = line.split(' ', 1)
        deps.append(pkg(name, ver, comment='Debian package in the build image (snapshot %s)' % snapshot,
                        purl='pkg:deb/debian/%s@%s?distro=trixie' % (name, ver.replace(':', '%3A').replace('+', '%2B'))))

    rels = [{'spdxElementId': 'SPDXRef-DOCUMENT', 'relationshipType': 'DESCRIBES', 'relatedSpdxElement': d['SPDXID']} for d in described]
    rels += [{'spdxElementId': d['SPDXID'], 'relationshipType': 'BUILD_DEPENDENCY_OF', 'relatedSpdxElement': described[0]['SPDXID']} for d in deps]
    doc = {'spdxVersion': 'SPDX-2.3', 'dataLicense': 'CC0-1.0', 'SPDXID': 'SPDXRef-DOCUMENT', 'name': 'img-pvr ddk113 build',
           'documentNamespace': 'https://github.com/bl00dclot/img-pvr/sbom/' + head,
           'creationInfo': {'created': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(epoch)), 'creators': ['Tool: scripts/gen-sbom.py']},
           'packages': described + deps, 'relationships': rels}
    json.dump(doc, sys.stdout, indent=1)
    print()


if __name__ == '__main__':
    main()
