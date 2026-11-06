#!/usr/bin/env python3
"""Stage the Adreno 732 kernel sources for the macOS 26.5 (ForSheng) kernel into boot/macos-gpu/src.

The base is our own iPadOS 27 (24A446) GPU build, the one that runs on this unit: the generated tree
UniXNU-a27/out/uke/ipados-27.0.1-24A446/uke_ipados27/tree, whose links point at UniXNU-a27 (drivers and kits) and at
UniXNU-uke-allenwdk/package (branch uke-a27-local, our zap pin). This copies exactly what that build compiled for the
GPU (UniXNU-uke-allenwdk scripts/build-adreno740-kits.py and build-native-legacy-display-27.py):

  src/drivers/Adreno740  tree/drivers/Adreno740 (*.cpp, *.h, *.inc; not the host tests)
  src/package            tree/kits/uke: GpuPowerProbe.cpp, GpuPowerBoard.h, ShengGmuVotes.{c,h}, BootLatencyTrace.h
  src/soc                tree/kits/Sm7675Platform: ShengGmu.{c,h}, ShengGmuCommandDb.{c,h}
  src/kits/<kit>         tree/kits/{AdrenoFirmware,ArmSmmu4K,AdrenoPM4} (*.c, *.h)
  src/generated          tree/build/sheng-gmu/*.h and tree/build/sheng-gpu-smmu/*.h, as that build generated them
  src/firmware.S         gen71100 GMU/SQE/ZAP as the strong native_adreno_*_image symbols (private: never commit)

Then patches/NNNN-*.patch are applied to the copies (patches/ipados17 holds the ones for the old iPadOS 17 base).
Our own macOS glue lives in src/macos and is not overwritten. Nothing in the source repositories changes.
Needs Python 3.11+, e.g. refs/macOS-ForSheng/.venv/bin/python.
Usage: refs/macOS-ForSheng/.venv/bin/python -I boot/macos-gpu/stage.py [--no-firmware]
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

D = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
CORE = D / 'UniXNU-a27'
TREE = CORE / 'out/uke/ipados-27.0.1-24A446/uke_ipados27/tree'
DEVICE = D / 'UniXNU-uke-allenwdk'
DEVICE_BRANCH = 'uke-a27-local'
SRC = HERE / 'src'
PACKAGE = ['GpuPowerProbe.cpp', 'GpuPowerBoard.h', 'ShengGmuVotes.c', 'ShengGmuVotes.h', 'BootLatencyTrace.h']
SOC = ['ShengGmu.c', 'ShengGmu.h', 'ShengGmuCommandDb.c', 'ShengGmuCommandDb.h']
KITS = ['AdrenoFirmware', 'ArmSmmu4K', 'AdrenoPM4']
GENERATED = ['build/sheng-gmu', 'build/sheng-gpu-smmu']
FIRMWARE = [('_native_adreno_firmware_image', 'gmu'), ('_native_adreno_sqe_firmware_image', 'sqe'),
            ('_native_adreno_zap_image', 'zap')]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git_head(path):
    return subprocess.run(['git', '-C', str(path), 'rev-parse', 'HEAD'], capture_output=True, text=True,
                          check=True).stdout.strip()


def fresh(path):
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)
    return path


def copy_files(source, destination, names):
    copied = {}
    for name in names:
        shutil.copy2(source / name, destination / name)
        copied[name] = sha(destination / name)
    return copied


def copy_tree(source, destination, patterns, skip=lambda p: False):
    fresh(destination)
    copied = {}
    for pattern in patterns:
        for path in sorted(source.glob(pattern)):
            if path.is_file() and not skip(path):
                shutil.copy2(path, destination / path.name)
                copied[path.name] = sha(path)
    return copied


def firmware_assembly(out):
    inputs = json.loads((DEVICE / 'scripts/data/uke-gpu-inputs.json').read_text())['firmware']
    lines = ['.section __DATA_CONST,__const']
    pins = {}
    for label, role in FIRMWARE:
        pin = inputs[role]
        path = DEVICE / '.cache/firmware/uke-gpu' / pin['name']
        blob = path.read_bytes()
        if len(blob) != pin['bytes'] or hashlib.sha256(blob).hexdigest() != pin['sha256']:
            sys.exit('firmware %s does not match its recorded pin' % pin['name'])
        lines += ['.p2align 12', '.globl ' + label, label + ':', '.incbin ' + json.dumps(str(path)), label + '_end:',
                  '.p2align 3', '.globl ' + label + '_size', label + '_size:', '.quad ' + label + '_end-' + label]
        pins[pin['name']] = dict(bytes=len(blob), sha256=pin['sha256'])
    (out / 'firmware.S').write_text('\n'.join(lines) + '\n')
    return pins


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--no-firmware', action='store_true', help='leave the weak empty firmware definitions in place')
    args = parser.parse_args()
    branch = subprocess.run(['git', '-C', str(DEVICE), 'rev-parse', '--abbrev-ref', 'HEAD'], capture_output=True,
                            text=True, check=True).stdout.strip()
    if branch != DEVICE_BRANCH:
        sys.exit('%s is on %s, not %s (our iPadOS 27 unit branch)' % (DEVICE, branch, DEVICE_BRANCH))
    if not (TREE / 'build/sheng-gmu/ShengGmuSource.h').is_file():
        sys.exit('%s has no generated GMU headers: build the iPadOS 27 kernel there first' % TREE)
    driver = (TREE / 'drivers/Adreno740').resolve()
    record = dict(base='iPadOS 27 24A446 unit build', tree=str(TREE), device_repo=str(DEVICE),
                  device_commit=git_head(DEVICE), core_commit=git_head(CORE), adreno740_commit=git_head(driver))
    (SRC / 'drivers/NativeAdreno').exists() and shutil.rmtree(SRC / 'drivers/NativeAdreno')   # the iPadOS 17 base
    record['adreno740'] = copy_tree(driver, SRC / 'drivers/Adreno740', ['*.cpp', '*.h', '*.inc'],
                                    skip=lambda p: p.name.endswith('Test.cpp'))
    record['package'] = copy_files(TREE / 'kits/uke', fresh(SRC / 'package'), PACKAGE)
    record['soc'] = copy_files(TREE / 'kits/Sm7675Platform', fresh(SRC / 'soc'), SOC)
    record['kits'] = {kit: copy_tree(TREE / 'kits' / kit, SRC / 'kits' / kit, ['*.c', '*.h'])
                      for kit in KITS}
    generated = fresh(SRC / 'generated')
    record['generated'] = {}
    for folder in GENERATED:
        for path in sorted((TREE / folder).glob('*.h')):
            shutil.copy2(path, generated / path.name)
            record['generated'][path.name] = sha(path)
    # Our macOS changes to the copied iPadOS sources (patches/NNNN-*.patch, in order); NativeUfs patches are not ours to apply.
    record['patches'] = []
    for patch in sorted((HERE / 'patches').glob('[0-9][0-9][0-9][0-9]-*.patch')):
        subprocess.run(['git', 'apply', '--whitespace=nowarn', str(patch)], cwd=SRC, check=True)
        record['patches'].append(patch.name)
    (SRC / 'firmware.S').unlink(missing_ok=True)
    if not args.no_firmware:
        record['firmware'] = firmware_assembly(SRC)
    (HERE / 'stage.json').write_text(json.dumps(record, indent=2) + '\n')
    print('staged Adreno740 %s (%d files), %d package + %d SoC files, kits %s, patches %d, firmware %s' % (
        record['adreno740_commit'][:7], len(record['adreno740']), len(record['package']), len(record['soc']),
        ', '.join(KITS), len(record['patches']), 'embedded' if not args.no_firmware else 'absent (weak)'))


if __name__ == '__main__':
    main()
