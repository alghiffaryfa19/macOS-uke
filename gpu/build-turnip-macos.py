#!/usr/bin/env python3
"""Build Turnip (Mesa freedreno Vulkan, native Darwin kernel backend) as arm64e for macOS 26.5.

Same recipe as our iPadOS 27 library (<tree>/scripts/build-turnip-darwin.py --target ios27, already arm64e with chained
fixups), with the macosx SDK and a macOS 26.5 deployment target instead of iphoneos 27.0. <tree> is our iPadOS 27 build
tree (UniXNU-a27/out/uke/ipados-27.0.1-24A446/uke_ipados27/tree); its pinned, already patched Mesa checkout
(.cache/mesa-work), venv and patch guard are reused. The kernel ABI headers come from boot/macos-gpu/src
(the staged Adreno740, the same files as the iPadOS 27 driver), so plugin, Turnip and kernel agree.

Output: <tree>/build/mesa-tu-macos/src/freedreno/vulkan/libvulkan_freedreno.dylib (never committed).
Usage: python3 -I boot/macos-gpu/build-turnip-macos.py [--configure-only]
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1] / 'UniXNU-a27/out/uke/ipados-27.0.1-24A446/uke_ipados27/tree'
SOURCE = TREE / '.cache/mesa-work'
OUT = TREE / 'build/mesa-tu-macos'
ABI = HERE / 'src/drivers/Adreno740'
DEPLOYMENT = '26.5'


def read(cmd):
    return subprocess.check_output([str(c) for c in cmd], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--configure-only', action='store_true')
    args = parser.parse_args()
    sys.path.insert(0, str(TREE / 'scripts'))
    from mesa_patch_identity import validate_recorded_patches
    pins = json.loads((TREE / 'scripts/data/adreno-compiler-inputs.json').read_text())
    if read(['git', '-C', SOURCE, 'rev-parse', 'HEAD']) != pins['mesa']['commit']:
        sys.exit('Pinned Mesa source differs')
    # macOS-only Turnip patches on top of the iPadOS 27 sequence: applied once to the shared checkout, then recorded.
    for patch in sorted((HERE / 'turnip').glob('*.patch')):
        applied = subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)], cwd=SOURCE,
                                 env=dict(os.environ, GIT_DIR='/dev/null'), capture_output=True)
        if applied.returncode:
            subprocess.run(['git', 'apply', str(patch)], cwd=SOURCE, env=dict(os.environ, GIT_DIR='/dev/null'), check=True)
        pins['patches'].append(str(patch))
    validate_recorded_patches(TREE, SOURCE, pins)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('IR3_', 'FD_MESA_', 'NIR_', 'MESA_SHADER_', 'TU_'))}
    env['PATH'] = str(TREE / '.cache/mesa-tools/bin') + ':' + str(TREE / '.cache/mesa-venv/bin') + ':' + env['PATH']
    sdk = read(['xcrun', '--sdk', 'macosx', '--show-sdk-path'])
    find = lambda tool: read(['xcrun', '--sdk', 'macosx', '--find', tool])
    flags = ['-target', 'arm64e-apple-macos' + DEPLOYMENT, '-isysroot', sdk, '-fstack-protector-strong']
    cross = OUT.with_name('mesa-tu-macos.cross')
    cross.parent.mkdir(parents=True, exist_ok=True)
    # A cross file even though the Mac runs the result's architecture: the generators must stay host arm64 tools.
    cross.write_text(
        '[binaries]\nc = %r\ncpp = %r\nobjc = %r\nar = %r\nstrip = %r\npkg-config = \'/usr/bin/false\'\n'
        % (find('clang'), find('clang++'), find('clang'), find('ar'), find('strip')) +
        "[host_machine]\nsystem = 'darwin'\ncpu_family = 'aarch64'\ncpu = 'arm64e'\nendian = 'little'\n"
        "[properties]\nneeds_exe_wrapper = true\n[built-in options]\n" +
        ''.join('%s = %r\n' % (k, flags + (['-Wl,-fixup_chains'] if k.endswith('link_args') else []))
                for k in ('c_args', 'cpp_args', 'objc_args', 'c_link_args', 'cpp_link_args', 'objc_link_args')))
    options = ['-Dgallium-drivers=', '-Dvulkan-drivers=freedreno', '-Dfreedreno-kmds=darwin',
               '-Dtu-darwin-abi-dir=' + str(ABI), '-Dplatforms=macos', '-Dglx=disabled',
               '-Degl=disabled', '-Dgbm=disabled', '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled',
               '-Dllvm=disabled', '-Dshared-glapi=disabled', '-Dxmlconfig=disabled', '-Dshader-cache=enabled',
               '-Dtools=', '-Dbuild-tests=false', '-Dvideo-codecs=', '-Dvulkan-layers=',
               '-Dlibunwind=disabled', '-Dlmsensors=disabled', '-Dvalgrind=disabled', '-Dexpat=disabled',
               '-Dzstd=disabled', '-Dbuildtype=release', '-Db_ndebug=true']
    # Meson caches cross files: an existing build directory gets the current link arguments explicitly (as the ios27 recipe).
    options += ['-D%s=%r' % (k, flags + ['-Wl,-fixup_chains']) for k in ('c_link_args', 'cpp_link_args', 'objc_link_args')]
    env.update(CC=find('clang'), CXX=find('clang++'), CFLAGS='', CXXFLAGS='', CPPFLAGS='', LDFLAGS='')
    # The pipeline-cache identity, computed as the iPadOS script does (target "macos-arm64e").
    source_diff = subprocess.check_output(['git', '-C', SOURCE, 'diff', 'HEAD', '--binary'])
    untracked = subprocess.check_output(['git', '-C', SOURCE, 'ls-files', '--others', '--exclude-standard', '-z']).split(b'\0')
    extras = {n.decode(): hashlib.sha256((SOURCE / n.decode()).read_bytes()).hexdigest()
              for n in untracked if n and (SOURCE / n.decode()).is_file()}
    profile = dict(mesa_revision=pins['mesa']['commit'], target='macos-arm64e',
                   source_diff_sha256=hashlib.sha256(source_diff).hexdigest(), extra_sources=extras,
                   patch_guard_sha256=hashlib.sha256((TREE / 'scripts/mesa_patch_identity.py').read_bytes()).hexdigest(),
                   compiler=read([env['CXX'], '--version']), options=options,
                   build_script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    source_id = hashlib.sha256(json.dumps(profile, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    options += ['-Dtu-builtin-source-id=' + source_id, '-Dtu-builtin-pack-header=']
    # The pipeline cache's identity from the compiler source alone, the same formula as the iPadOS 27 recipe.
    options += ['-Dtu-build-id=' + hashlib.sha256(json.dumps(dict(
        mesa_revision=pins['mesa']['commit'], source_diff_sha256=profile['source_diff_sha256'], extra_sources=extras),
        sort_keys=True).encode()).hexdigest()[:40]]
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'builtin-build-profile.json').write_text(json.dumps(dict(source_id=source_id, profile=profile), indent=2) + '\n')
    meson = TREE / '.cache/mesa-venv/bin/meson'
    command = ([meson, 'setup', OUT, SOURCE, '--wrap-mode=nodownload', '--cross-file', cross] + options
               if not (OUT / 'build.ninja').exists() else [meson, 'configure', OUT] + options)
    log = TREE / '.cache/mesa-tu-macos-configure.log'
    with log.open('w') as stream:
        if subprocess.run([str(c) for c in command], env=env, stdout=stream, stderr=subprocess.STDOUT).returncode:
            sys.exit('Configure failed; see %s\n%s' % (log, '\n'.join(log.read_text().splitlines()[-12:])))
    if args.configure_only:
        print('PASS: Turnip darwin-kmd configuration (macOS arm64e)')
        return
    log = TREE / '.cache/mesa-tu-macos-build.log'
    with log.open('w') as stream:
        result = subprocess.run(['ninja', '-C', str(OUT), '-j6'], env=env, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        sys.exit('Build failed; see %s\n%s' % (log, '\n'.join(
            l for l in log.read_text().splitlines() if ': error:' in l or l.startswith('FAILED:'))[:3000]))
    library = OUT / 'src/freedreno/vulkan/libvulkan_freedreno.dylib'
    if read(['lipo', '-archs', library]) != 'arm64e':
        sys.exit('%s is not arm64e' % library)
    if ' T _vk_icdGetInstanceProcAddr' not in read(['nm', '-gU', library]):
        sys.exit('The library does not export vk_icdGetInstanceProcAddr')
    print('PASS: Turnip (darwin kernel backend) for macOS arm64e, %d bytes' % library.stat().st_size)


if __name__ == '__main__':
    main()
