#!/usr/bin/env python3
"""Build corecrypto's kext C sources as an arm64e kernel archive, the way macOS-ForSheng's
build_macos26_crypto.py does: text moved to __TEXT_EXEC,__cc26_text and symbols that collide with
the kernel archive renamed to _sheng_cc26_*."""
import concurrent.futures
import json
import re
import subprocess
import sys
from pathlib import Path

FS = Path(sys.argv[1])
OUT = FS / 'out/macos26-xnu-arm64e'
SDK = OUT / 'kernel-sdk'
SOURCE = FS / 'sources/macos26/corecrypto'
WORK = OUT / 'corecrypto'
WORK.mkdir(exist_ok=True)


def capture(*cmd):
    return subprocess.check_output(cmd, text=True).strip()


def names(path, mode):
    return {l.strip() for l in capture('xcrun', 'nm', '-g', mode, '-j', str(path)).splitlines() if l.startswith('_')}


subprocess.run(['plutil', '-convert', 'json', '-o', WORK / 'project.json',
                SOURCE / 'corecrypto.xcodeproj/project.pbxproj'], check=True)
objects = json.loads((WORK / 'project.json').read_text())['objects']
parents = {c: k for k, v in objects.items() for c in v.get('children', [])}


def resolve(key):
    e = objects[key]
    if e.get('sourceTree') == 'SOURCE_ROOT' or key not in parents:
        return SOURCE / e.get('path', '')
    return resolve(parents[key]) / e.get('path', '')


target = next(v for v in objects.values() if v.get('isa') == 'PBXNativeTarget' and v.get('name') == 'corecrypto_kext')
phase = next(objects[k] for k in target['buildPhases'] if objects[k]['isa'] == 'PBXSourcesBuildPhase')
sources = [resolve(objects[k]['fileRef']) for k in phase['files']
           if objects[objects[k]['fileRef']].get('path', '').endswith('.c')]
include = WORK / 'include/corecrypto'
include.mkdir(parents=True, exist_ok=True)
for h in SOURCE.glob('*/corecrypto/*.h'):
    if not (include / h.name).exists():
        (include / h.name).symlink_to(h)
fw = SDK / 'System/Library/Frameworks/Kernel.framework/Versions/A'
base = ['xcrun', '--sdk', 'macosx', 'clang', '-target', 'arm64e-apple-macos26.5',
        '-isysroot', capture('xcrun', '--sdk', 'macosx', '--show-sdk-path'),
        '-mkernel', '-ffreestanding', '-fno-builtin', '-fno-stack-check', '-std=gnu99', '-fblocks', '-O2', '-g',
        '-fno-vectorize', '-fno-slp-vectorize', '-mno-implicit-float', '-DKERNEL=1', '-DKERNEL_PRIVATE=1',
        '-DBUILDKERNEL=1', '-DCC_USE_ASM=0', '-DNDEBUG=1', '-DCC_DISABLE_RSAKEYGEN=1',
        '-Wno-deprecated-declarations', '-Wno-incompatible-library-redeclaration',
        '-I' + str(include.parent), '-I' + str(fw / 'PrivateHeaders'), '-I' + str(fw / 'Headers'),
        '-I' + str(SDK / 'usr/local/include')]
for folder in sorted({h.parent for h in SOURCE.rglob('*.h') if 'test' not in str(h.relative_to(SOURCE)).lower()}):
    base += ['-I', str(folder)]
kernel_names = names(OUT / 'obj/DEVELOPMENT_ARM64_SHENG/libkernel.development.sheng.a', '-U')


def variant(name, extra):
    objdir = WORK / name
    objdir.mkdir(exist_ok=True)

    def one(path):
        stem = re.sub(r'\W', '_', str(path.relative_to(SOURCE)))
        asm, obj = objdir / (stem + '.s'), objdir / (stem + '.o')
        r = subprocess.run(base + extra + ['-S', str(path), '-o', str(asm)], capture_output=True, text=True)
        if r.returncode:
            return path, r.stderr[-1500:]
        asm.write_text(re.sub(r'(\.section\s+)__TEXT,__text(?=[,\s])', r'\1__TEXT_EXEC,__cc26_text', asm.read_text()))
        r = subprocess.run(['xcrun', 'clang', '-target', 'arm64e-apple-macos26.5', '-mkernel', '-c', '-x',
                            'assembler', str(asm), '-o', str(obj)], capture_output=True, text=True)
        return (obj if r.returncode == 0 else path), (r.stderr[-1500:] if r.returncode else '')

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        results = list(ex.map(one, sources))
    bad = [(str(p), e) for p, e in results if e]
    if bad:
        print(f'{name}: {len(bad)} failures', bad[:2])
        sys.exit(1)
    archive = WORK / f'libcorecrypto-{name}.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, *[p for p, _ in results]], check=True)
    print(f'{name}: {len(results)} files ok')
    return archive


plain = variant('plain', [])
collisions = sorted(names(plain, '-U') & kernel_names)
namespace = WORK / 'namespace.h'
namespace.write_text(''.join(f'#pragma redefine_extname {n[1:]} _sheng_cc26_{n[1:]}\n' for n in collisions))
archive = variant('native', ['-include', str(namespace)])
print('renamed', len(collisions), 'colliding symbols; duplicates left:', len(names(archive, '-U') & kernel_names))
print(archive)
