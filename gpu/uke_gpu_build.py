#!/usr/bin/env python3
"""Compile the staged Adreno 732 driver (boot/macos-gpu/src) into the NativeUfs bundle of the macOS 26.5 kernel.

build_macos26_storage.py calls compile_objects() next to its UKE_TOUCH_SRC block (environment UKE_GPU_SRC), with the
same compile command it built for NativeUfs.cpp, and links the returned objects into NativeUfs.macho. personalities()
gives the IOKit personality to add to that bundle's Info.plist.

Standalone (no XNU build, nothing under refs/macOS-ForSheng/out is written):
    python3 -I boot/macos-gpu/uke_gpu_build.py --out <dir>
compiles every object against the recorded XNU compile record, links a test kext, and reports its size and the
symbols it imports from the kernel.
"""
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
D = HERE.parents[1]
FS = D / 'refs/macOS-ForSheng'
XNU = FS / 'out/macos26-xnu-arm64e'
CXX_ONLY = {'-std=gnu++2b', '-fsized-deallocation', '-fapple-kext', '-fc++-static-destructors=none'}
# The iPadOS 27 unit build's GPU objects (UniXNU-uke-allenwdk scripts/build-adreno740-kits.py plus GpuPowerProbe from
# build-native-legacy-display-27.py), minus the iOS-only pieces:
#   NativeAdrenoAccelerator.cpp  IOAcceleratorES subclass for iOS discovery (macos/UkeGpuAccelerator.cpp replaces it)
#   NativeAdrenoClient.cpp       its user-visible accelerator client, same reason
SOC_C = ['ShengGmu.c', 'ShengGmuCommandDb.c']
PACKAGE_C = ['ShengGmuVotes.c']
KIT_C = {'AdrenoFirmware': ['AdrenoHfi.c', 'AdrenoHfiStartup.c', 'AdrenoHfiVotes.c', 'AdrenoFirmware.c'],
         'AdrenoPM4': ['AdrenoPM4.c']}
EXCLUDED = {'NativeAdrenoAccelerator.cpp', 'NativeAdrenoClient.cpp'}
# UKE_GPU_FIRMWARE: drv (default) reads gen71100 from the DRV window at boot and needs the NativeUfs DRV patch
# (boot/macos-gpu/patches/nativeufs-drv-window.patch); embed links firmware.S into the kernel (about 150 KiB more than
# vendor_boot holds today); none leaves the weak empty images, so the probe stops before the GMU cold start.
FIRMWARE_USERS = {'GpuPowerProbe.cpp', 'NativeAdrenoService.cpp'}
# Defined by other objects linked into the same NativeUfs bundle, so the static link resolves them (weak or not):
# the touch driver's report counters, which GpuPowerProbe.cpp reads to raise the GPU clock on touch.
BUNDLE_PROVIDED = {'_native_hid_input_events': 'boot/macos-touch UkeTouchPointer.cpp'}


def firmware_mode():
    mode = os.environ.get('UKE_GPU_FIRMWARE', 'drv')
    if mode not in ('drv', 'embed', 'none'):
        raise ValueError('UKE_GPU_FIRMWARE must be drv, embed or none')
    return mode


def sources(src):
    items = [src / 'soc' / name for name in SOC_C] + [src / 'package' / name for name in PACKAGE_C]
    for kit, names in KIT_C.items():
        items += [src / 'kits' / kit / name for name in names]
    items += sorted((src / 'kits/ArmSmmu4K').glob('ArmSmmu4K*.c'))
    items += sorted(p for p in (src / 'drivers/Adreno740').glob('NativeAdreno*.cpp') if p.name not in EXCLUDED)
    items += [src / 'package/GpuPowerProbe.cpp', src / 'macos/UkeGpu.cpp', src / 'macos/UkeGpuAccelerator.cpp']
    return items


def includes(src):
    # Same order as the iPadOS 27 build: the board package before the SoC layer.
    paths = [src / 'macos', src / 'package', src / 'soc', src / 'generated', src / 'drivers/Adreno740']
    paths += [src / 'kits' / kit for kit in ('AdrenoFirmware', 'AdrenoPM4', 'ArmSmmu4K')]
    return [x for p in paths for x in ('-I', str(p))]


def build_id(src):
    # As UniXNU-uke-allenwdk scripts/build-adreno740-kits.py: the driver's trace id is the hash of its Native* files.
    digest = hashlib.sha256()
    for path in sorted((src / 'drivers/Adreno740').glob('Native*')):
        if path.is_file():
            digest.update(path.name.encode())
            digest.update(path.read_bytes())
    return '-DNATIVE_GPU_BUILD_ID=0x%sULL' % digest.hexdigest()[:16]


def compile_objects(command, wrapper, obj, destination, run, record, compile_record, env, sdk, src=None, standalone=False):
    """Objects for the NativeUfs link. `command` is the NativeUfs.cpp compile command of build_macos26_storage.py."""
    src = Path(src or os.environ['UKE_GPU_SRC'])
    if firmware_mode() == 'drv' and not os.environ.get('UKE_DRV_CONFIG') and not standalone:
        raise ValueError('UKE_GPU_FIRMWARE=drv needs the DRV window (UKE_DRV_CONFIG and nativeufs-drv-window.patch)')
    if not standalone and not os.environ.get('UKE_TOUCH_SRC'):
        raise ValueError('UKE_GPU_SRC needs UKE_TOUCH_SRC too: the touch objects define %s' % ', '.join(BUNDLE_PROVIDED))
    out = destination / 'uke-gpu'
    out.mkdir(parents=True, exist_ok=True)
    objects = []
    for source in sources(src):
        out_obj = out / (source.stem + ('.kit.o' if source.suffix == '.c' else '.o'))
        c_mode = source.suffix == '.c'
        extra = []
        for arg in command:
            if arg == str(wrapper):
                extra.append(str(source))
            elif arg == str(obj):
                extra.append(str(out_obj))
            elif c_mode and arg == '-xc++':
                extra.append('-xc')
            elif c_mode and arg == '--driver-mode=g++':
                extra.append('--driver-mode=gcc')
            elif c_mode and arg in CXX_ONLY:
                continue
            else:
                extra.append(arg)
        # The iPadOS build used -Werror with these exceptions; keep the warnings visible but never fatal here.
        extra += ['-Wno-error', '-Wno-unused-variable', '-Wno-sign-compare', '-Wno-missing-field-initializers',
                  '-Wno-unused-function', '-Wno-overloaded-virtual', build_id(src), *includes(src)]
        if firmware_mode() == 'drv':
            if source.name in FIRMWARE_USERS:
                extra += ['-include', str(src / 'macos/UkeGpuFirmwareRemap.h')]
            if source.name == 'UkeGpu.cpp':
                drv = json.loads(Path(os.environ.get('UKE_DRV_CONFIG') or HERE / 'drv.json').read_text())
                extra += ['-DUKE_GPU_FIRMWARE_DRV=1', '-DUKE_DRV_FIRMWARE_OFFSET=%#xULL' % drv['firmware_offset']]
        if source.name == 'UkeGpuAccelerator.cpp' and os.environ.get('UKE_GPU_IOACCELERATOR', 'own') == 'own':
            extra += ['-DUKE_GPU_OWN_IOACCELERATOR=1']   # IOGraphicsFamily is not in the boot collection
        run(extra, 'ufs-driver-gpu-' + source.stem, record, cwd=Path(compile_record['directory']), env=env)
        objects.append(str(out_obj))
    firmware = src / 'firmware.S'
    if firmware_mode() == 'embed':
        if not firmware.is_file():
            raise ValueError('UKE_GPU_FIRMWARE=embed needs src/firmware.S (stage.py without --no-firmware)')
        fw_obj = out / 'firmware.o'
        run(['xcrun', 'clang', '-arch', 'arm64e', '-mmacos-version-min=26.5', '-c', str(firmware), '-o', str(fw_obj)],
            'ufs-driver-gpu-firmware', record, env=env)
        objects.append(str(fw_obj))
    return objects


def personalities(bundle):
    """UkeGpu starts on IOResources like UkeTouchPointer and runs the staged power/firmware bring-up (sheng-gpu-power)."""
    return {'UkeGpu': dict(CFBundleIdentifier=bundle, IOClass='UkeGpu', IOProviderClass='IOResources',
                           IOResourceMatch='IOKit', IOMatchCategory='UkeGpu', IOProbeScore=500)}


# ------------------------------------------------------------------------------------------------- standalone check

def storage_command(compile_record, wrapper, obj, sdk):
    """The NativeUfs.cpp command exactly as build_macos26_storage.build() assembles it (ufs transport)."""
    arguments = compile_record['arguments']
    command, index = [], 0
    while index < len(arguments):
        arg = arguments[index]
        if arg == compile_record['file']:
            command.append(str(wrapper))
        elif arg in ('-o', '-MF', '-MT', '-MQ', '-MJ'):
            if arg == '-o':
                command += ['-o', str(obj)]
            index += 1
        elif arg == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif arg.startswith('-W'):
            pass
        elif arg not in ('-MMD', '-MD', '-MP'):
            command.append(arg)
        index += 1
    command[1:1] = ['-I', str(XNU / 'ufs-driver/include')]
    command[1:1] = ['-DNATIVE_UFS_ROOT_PARTITION_NAME="userdata"']
    command += ['-I', str(XNU / 'source/osfmk'), '-I', str(sdk / 'usr/include'),
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list',
                '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier']
    return command


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--src', type=Path, default=HERE / 'src')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    recipe = XNU / 'obj/DEVELOPMENT_ARM64_SHENG/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    compile_record = json.loads(recipe.read_text().strip().rstrip(','))
    sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    wrapper, obj = args.out / 'NativeUfs.cpp', args.out / 'NativeUfs.o'
    command = storage_command(compile_record, wrapper, obj, sdk)
    failures = []

    def run(cmd, name, record, cwd=None, env=None):
        result = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
        (args.out / (name + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            failures.append(name)
            print('FAIL %s (see %s.log)' % (name, name))

    objects = compile_objects(command, wrapper, obj, args.out, run, {}, compile_record, None, sdk, args.src, standalone=True)
    if failures:
        sys.exit('%d of %d objects failed' % (len(failures), len(objects)))
    binary = args.out / 'UkeGpuTest.macho'
    run(['xcrun', 'ld', '-arch', 'arm64e', '-kext', '-kexts_use_stubs', '-platform_version', 'macos', '26.5', '26.5',
         '-undefined', 'dynamic_lookup', '-fixup_chains', '-no_adhoc_codesign', '-headerpad', '0x1000',
         *objects, '-o', str(binary)], 'link', {})
    if failures:
        sys.exit('link failed')
    undefined = subprocess.run(['xcrun', 'nm', '-u', str(binary)], capture_output=True, text=True).stdout.split()
    (args.out / 'imports.txt').write_text('\n'.join(undefined) + '\n')
    # What native_storage_module.prepare() would refuse: a weak import, or a name the kernel does not define.
    # (The selected Apple kexts also export symbols; the GPU objects import none of theirs.)
    kernel = set(subprocess.run(['xcrun', 'nm', '-gUj', str(XNU / 'kernel.development.sheng')],
                                capture_output=True, text=True).stdout.split())
    provided = set(BUNDLE_PROVIDED)
    if firmware_mode() == 'drv':
        provided.add('_native_ufs_drv_read')   # the NativeUfs DRV patch
    missing = sorted(set(undefined) - kernel - provided)
    listing = subprocess.run(['xcrun', 'nm', '-m', '-u', str(binary)], capture_output=True, text=True).stdout
    weak = sorted(set(line.split()[-4] for line in listing.splitlines() if 'weak' in line) - provided)
    size = binary.stat().st_size
    print('%s: %d objects, test kext %d bytes (%.2f MiB), %d kernel imports (imports.txt), firmware %s'
          % ('PASS' if not missing and not weak else 'FAIL', len(objects), size, size / 1048576, len(undefined),
             firmware_mode()))
    if missing:
        print('  not defined by the macOS kernel: ' + ' '.join(missing))
    if weak:
        print('  weak imports (the prelinker refuses them): ' + ' '.join(weak))
    if missing or weak:
        sys.exit(1)


if __name__ == '__main__':
    main()
