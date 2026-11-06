#!/usr/bin/env python3
"""Build the NativeAdrenoMetal plugin bundle for macOS 26.5 (arm64e) and stage it for the DRV volume.

The base is our iPadOS 27 GPU userland (<tree>/scripts/build-adreno-metal-plugin.py --target ios27, AdrenoMetal 854f99a,
already arm64e), where <tree> is UniXNU-a27/out/uke/ipados-27.0.1-24A446/uke_ipados27/tree. Nothing in it is changed:

  1. Turnip      boot/macos-gpu/build-turnip-macos.py  -> <tree>/build/mesa-tu-macos/.../libvulkan_freedreno.dylib
  2. bridge      <tree>/scripts/build-adreno-metal-bridge.py run with one more target, macos (arm64e-apple-darwin, std from
                 rust-src with -Z build-std as for ios27, MACOSX_DEPLOYMENT_TARGET 26.5): same translator pin, 30 patches
                 and runtime id -> <tree>/build/adreno-metal-bridge/macos/libadreno_metal_bridge.a
  3. plugin      drivers/AdrenoMetal copied, plugin/*.patch applied (WindowServer instead of backboardd, IOAccelerator
                 instead of IOAcceleratorES for the plugin and Turnip, no iPadOS 27 QuartzCore address writes), compiled
                 with the kernel ABI headers of boot/macos-gpu/src; the Rust objects go through the same PAC boundary
                 (RustPacBoundary.c, rust-pac-rewrite.py) as on iPadOS 27
  4. bundle      NativeAdrenoMetal.bundle (macOS layout, Turnip in Contents/MacOS, ad-hoc signed)
                 -> <tree>/build/adreno-metal-plugin-macos

--stage DIR copies the bundle into a DRV staging folder (make-drv.py image DIR ...), with NativeAdrenoMetal.publish beside
it when --publish is given (WindowServer then returns the native device instead of running the stage-1 user test).
Usage: python3 -I boot/macos-gpu/build-plugin-macos.py [--objects-only] [--skip-bridge-build] [--stage DIR [--publish]]
"""
import argparse
import hashlib
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1] / 'UniXNU-a27/out/uke/ipados-27.0.1-24A446/uke_ipados27/tree'
OUT = TREE / 'build/adreno-metal-plugin-macos'
ABI = HERE / 'src/drivers/Adreno740'
TURNIP = TREE / 'build/mesa-tu-macos/src/freedreno/vulkan/libvulkan_freedreno.dylib'
BRIDGE = TREE / 'build/adreno-metal-bridge/macos/libadreno_metal_bridge.a'
TARGET = ['-target', 'arm64e-apple-macos26.5']
# As the ios27 plugin build: the Rust objects' imports that take a callback go through RustPacBoundary.c's signing wrappers.
PAC_RENAMES = {'_pthread_create': '_nmb_pac_pthread_create', '__tlv_atexit': '_nmb_pac_tlv_atexit', '_atexit': '_nmb_pac_atexit',
               '__dyld_register_func_for_add_image': '_nmb_pac_dyld_register_func_for_add_image',
               '__Unwind_Backtrace': '_nmb_pac_Unwind_Backtrace', '_dlsym': '_nmb_pac_dlsym'}
# The iPadOS 27 bridge script, given a macos target. Each edit must match exactly once, so a changed script stops here.
BRIDGE_EDITS = [
    ("choices=['ios', 'ios27', 'host']", "choices=['ios', 'ios27', 'host', 'macos']"),
    ("'ios27': 'arm64e-apple-ios', 'host'", "'ios27': 'arm64e-apple-ios', 'macos': 'arm64e-apple-darwin', 'host'"),
    ("['-Z', 'build-std=std,panic_unwind'] if args.target == 'ios27' else []",
     "['-Z', 'build-std=std,panic_unwind'] if args.target in ('ios27', 'macos') else []"),
    ("MACOSX_DEPLOYMENT_TARGET='14.0')", "MACOSX_DEPLOYMENT_TARGET='26.5' if args.target == 'macos' else '14.0')"),
    ("if args.target == 'ios27':\n    env['RUSTC_BOOTSTRAP'] = '1'", "if args.target in ('ios27', 'macos'):\n    env['RUSTC_BOOTSTRAP'] = '1'"),
]


def run(command, **kw):
    subprocess.run([str(c) for c in command], check=True, **kw)


def archs(path):
    return subprocess.check_output(['lipo', '-archs', str(path)], text=True).strip()


def build_bridge():
    sysroot = Path(subprocess.check_output(['rustc', '+1.98.0', '--print', 'sysroot'], text=True).strip())
    if not (sysroot / 'lib/rustlib/src/rust/library').is_dir():
        sys.exit('rust-src is not installed for 1.98.0 (arm64e-apple-darwin has no prebuilt std).\n'
                 'Ask before downloading: rustup component add rust-src --toolchain 1.98.0-aarch64-apple-darwin')
    script = TREE / 'scripts/build-adreno-metal-bridge.py'
    text = script.read_text()
    for old, new in BRIDGE_EDITS:
        if text.count(old) != 1:
            sys.exit('%s changed: %r does not match once' % (script, old))
        text = text.replace(old, new)
    # Run as that script (its ROOT is the tree), in a child so its exits and globals stay its own.
    code = ('import sys; sys.argv=[%r, "--target", "macos"]; sys.path.insert(0, %r); '
            'exec(compile(%r, %r, "exec"), {"__file__": %r, "__name__": "__main__"})'
            % (str(script), str(script.parent), text, str(script), str(script)))
    run([sys.executable, '-c', code], cwd=TREE)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--objects-only', action='store_true', help='compile the plugin and frontend objects and stop')
    parser.add_argument('--skip-bridge-build', action='store_true', help='use an existing %s' % BRIDGE)
    parser.add_argument('--stage', type=Path)
    parser.add_argument('--publish', action='store_true')
    args = parser.parse_args()
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    if OUT.exists():
        shutil.rmtree(OUT)
    # Same relative layout as the tree: GpuScanoutPresenter.h includes ../GopFramebuffer/NativeGOPWire.h.
    src = OUT / 'drivers/AdrenoMetal'
    shutil.copytree((TREE / 'drivers/AdrenoMetal').resolve(), src, ignore=shutil.ignore_patterns('.git', 'tools'))
    (OUT / 'drivers/GopFramebuffer').symlink_to((TREE / 'drivers/GopFramebuffer').resolve())
    patches = sorted((HERE / 'plugin').glob('[0-9][0-9][0-9][0-9]-*.patch'))
    for patch in patches:
        run(['git', 'apply', '--whitespace=nowarn', patch], cwd=src)
    source_hash = hashlib.sha256()
    for path in sorted(src.rglob('*')):
        if path.is_file() and path.suffix in ('.h', '.mm', '.cpp', '.rs', '.patch'):
            source_hash.update(str(path.relative_to(src)).encode())
            source_hash.update(path.read_bytes())
    base = ['xcrun', 'clang', *TARGET, '-isysroot', sdk, '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-fstack-protector-strong',
            '-DNATIVE_METAL_BUILD_ID="%s"' % source_hash.hexdigest()]
    # The smoke shaders header the iPadOS 27 build generated (gen-turnip-smoke-shaders.py); no host replay capture
    # (TurnipReplay.h is optional and records an iPadOS submission).
    smoke = OUT / 'turnip-smoke'
    smoke.mkdir()
    shutil.copy2(TREE / 'build/turnip-smoke/NativeTurnipShaders.h', smoke)
    include = ['-I', ABI, '-I', TREE / 'kits/AdrenoPM4', '-I', TREE / '.cache/mesa-work/include', '-I', smoke, '-I', src]
    pm4 = OUT / 'AdrenoPM4.o'
    run([*base, '-std=c11', '-c', TREE / 'kits/AdrenoPM4/AdrenoPM4.c', '-o', pm4])
    boundary = OUT / 'RustPacBoundary.o'
    run([*base, '-std=c11', '-I', TREE / '.cache/mesa-work/include', '-c', src / 'bridge/RustPacBoundary.c', '-o', boundary])
    objects = []
    for name in ('NativeAdrenoMetalPlugin', 'NativeBridgeMetalFrontend'):
        obj = OUT / (name + '.o')
        run(['xcrun', 'clang++', *base[2:], '-std=c++17', '-fobjc-arc', '-Wno-deprecated-declarations', *include,
             '-x', 'objective-c++', '-c', src / (name + '.mm'), '-o', obj])
        objects.append(obj)
    print('objects: plugin, frontend, PM4 and PAC boundary compile for arm64e macOS 26.5 (%d patches)' % len(patches))
    if args.objects_only:
        return
    if not TURNIP.is_file() or archs(TURNIP) != 'arm64e':
        sys.exit('Build Turnip first: python3 -I boot/macos-gpu/build-turnip-macos.py')
    if not args.skip_bridge_build:
        build_bridge()
    if not BRIDGE.is_file() or archs(BRIDGE) != 'arm64e':
        sys.exit('no arm64e bridge at %s' % BRIDGE)
    objcopy = next(p for p in ['/opt/homebrew/opt/llvm/bin/llvm-objcopy', shutil.which('llvm-objcopy') or ''] if p and Path(p).exists())
    pac_bridge = OUT / 'libadreno_metal_bridge_pac.a'
    run([objcopy, *['--redefine-sym=%s=%s' % pair for pair in PAC_RENAMES.items()], BRIDGE, pac_bridge])
    bundle = OUT / 'NativeAdrenoMetal.bundle'
    (bundle / 'Contents/MacOS').mkdir(parents=True)
    exe = bundle / 'Contents/MacOS/NativeAdrenoMetal'
    link_map = OUT / 'NativeAdrenoMetal.map'
    run(['xcrun', 'clang++', *base[2:], '-bundle', *objects, pm4, pac_bridge, boundary, '-Wl,-map,' + str(link_map),
         '-Wl,-dead_strip', '-lobjc', '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
         '-framework', 'CoreFoundation', '-o', exe])
    run([sys.executable, '-I', TREE / 'scripts/rust-pac-rewrite.py', exe, link_map, pac_bridge.name])
    run(['strip', '-x', exe])
    symbols = OUT / 'NativeAdrenoMetal.dSYM'
    if (bundle / 'Contents/MacOS/NativeAdrenoMetal.dSYM').exists():
        shutil.move(bundle / 'Contents/MacOS/NativeAdrenoMetal.dSYM', symbols)
    # Turnip rides beside the plugin executable (turnip::libraryPath: dladdr of the plugin, then libvulkan_freedreno.dylib).
    turnip = bundle / 'Contents/MacOS/libvulkan_freedreno.dylib'
    shutil.copyfile(TURNIP, turnip)
    run(['strip', '-x', turnip])
    run(['codesign', '--force', '--sign', '-', '--identifier', 'org.blu-sharky.turnip', turnip])
    (bundle / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier='org.blu-sharky.NativeAdrenoMetal', CFBundleExecutable='NativeAdrenoMetal', CFBundlePackageType='BNDL',
        CFBundleVersion='1', CFBundleShortVersionString='1', NSPrincipalClass='NativeAdrenoMetalDevice',
        CFBundleSupportedPlatforms=['MacOSX'], LSMinimumSystemVersion='26.5'), fmt=plistlib.FMT_XML))
    (OUT / 'gpu-build.json').write_text(json.dumps(dict(
        metal_source_sha256=source_hash.hexdigest(), target='macos-arm64e', patches=[p.name for p in patches],
        turnip=json.loads((TURNIP.parents[3] / 'builtin-build-profile.json').read_text())), indent=2) + '\n')
    run(['codesign', '--force', '--sign', '-', '--identifier', 'org.blu-sharky.NativeAdrenoMetal', bundle])
    run(['codesign', '--verify', '--strict', bundle])
    for binary in (exe, turnip):
        if archs(binary) != 'arm64e':
            sys.exit('%s is not arm64e' % binary)
    detail = subprocess.run(['codesign', '-dvvv', str(bundle)], capture_output=True, text=True).stderr
    print('bundle %s CDHash %s' % (bundle, re.search(r'^CDHash=(\w+)$', detail, re.M)[1]))
    if args.stage:
        target = args.stage / 'NativeAdrenoMetal.bundle'
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(bundle, target, symlinks=True)
        (args.stage / 'NativeAdrenoMetal.publish').unlink(missing_ok=True)
        if args.publish:
            (args.stage / 'NativeAdrenoMetal.publish').write_text('')
        print('staged into %s%s' % (args.stage, ' (published)' if args.publish else ' (stage-1 user test)'))


if __name__ == '__main__':
    main()
