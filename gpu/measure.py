#!/usr/bin/env python3
"""How many bytes the GPU objects add to the prelinked macOS kernel (read-only on refs/macOS-ForSheng/out).

emit_macos26_security.py copies every NativeUfs segment at its full VM size (zerofill and __LINKEDIT included) into
__NVB_* / __PRELINK_TEXT, so the growth of the kernel file is the growth of the bundle's summed segment VM sizes.
This links the current NativeUfs objects (NativeUfs.o + touch-*.o from the last build) alone and with the GPU objects
of `uke_gpu_build.py --out DIR`, with and without the embedded firmware and with ld -x (no local symbols), and prints
the difference next to the free space of the FAT inside the current vendor_boot.
Usage: python3 -I boot/macos-gpu/measure.py DIR
"""
import struct
import subprocess
import sys
from pathlib import Path

D = Path(__file__).resolve().parents[2]
UFS = D / 'refs/macOS-ForSheng/out/macos26-xnu-arm64e/ufs-driver'
LINK = ['xcrun', 'ld', '-arch', 'arm64e', '-kext', '-kexts_use_stubs', '-platform_version', 'macos', '26.5', '26.5',
        '-undefined', 'dynamic_lookup', '-fixup_chains', '-no_adhoc_codesign', '-headerpad', '0x1000']


def segments(path):
    d = path.read_bytes()
    count, cursor, out = struct.unpack_from('<I', d, 16)[0], 32, {}
    for _ in range(count):
        kind, length = struct.unpack_from('<II', d, cursor)
        if kind == 0x19:
            out[d[cursor + 8:cursor + 24].rstrip(b'\0').decode()] = struct.unpack_from('<Q', d, cursor + 32)[0]
        cursor += length
    return out


def fat_free():
    fat = D / 'boot/out/macos_payload.fat'
    text = subprocess.run(['mdir', '-i', str(fat), '::/EFI/UniXNU/Payloads'], capture_output=True, text=True,
                          env={'MTOOLS_SKIP_CHECK': '1', 'PATH': '/opt/homebrew/bin:/usr/bin:/bin'}).stdout
    for line in text.splitlines():
        if 'bytes free' in line:
            return int(line.split('bytes')[0].replace(' ', '').replace(' ', ''))
    return None


def main():
    work = Path(sys.argv[1])
    base = [str(UFS / 'NativeUfs.o')] + sorted(str(p) for p in UFS.glob('touch-*.o'))
    gpu = sorted(str(p) for p in (work / 'uke-gpu').glob('*.o'))
    nofw = [p for p in gpu if not p.endswith('/firmware.o')]
    variants = {'base': (base, []), 'gpu': (base + gpu, []), 'gpu -x': (base + gpu, ['-x']),
                'gpu, fw in DRV': (base + nofw, []), 'gpu, fw in DRV, -x': (base + nofw, ['-x'])}
    sizes = {}
    for name, (objects, flags) in variants.items():
        out = work / ('measure-' + name.replace(' ', '').replace(',', '_') + '.macho')
        subprocess.run(LINK + flags + objects + ['-o', str(out)], check=True)
        seg = segments(out)
        sizes[name] = sum(seg.values())
        print('%-20s %s total %dK' % (name, ' '.join('%s=%dK' % (k.strip('_'), v // 1024) for k, v in seg.items()),
                                      sizes[name] // 1024))
    free = fat_free()
    # The newest macOS vendor_boot is the one built from boot/out/macos_payload.fat.
    image = max((D / 'boot/out').glob('vendor_boot_uke_macos_*.img'), key=lambda p: p.stat().st_mtime)
    print('FAT free now: %s bytes; vendor_boot partition spare: %d bytes (%s; FAT can grow by about that)'
          % (free, 100663296 - image.stat().st_size, image.name))
    for name in list(variants)[1:]:
        grow = sizes[name] - sizes['base']
        print('%-20s +%d bytes (%.0fK)%s' % (name, grow, grow / 1024, '' if free is None else
                                             '  fits' if grow <= free else '  does NOT fit the current FAT'))


if __name__ == '__main__':
    main()
