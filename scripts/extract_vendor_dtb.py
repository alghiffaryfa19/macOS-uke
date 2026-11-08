#!/usr/bin/env python3
"""Extract the board DTB from a stock Android vendor_boot image (header v3/v4).

usage: extract_vendor_dtb.py vendor_boot_a.img out.dtb
Vendor_boot may carry several concatenated DTBs; the one whose root compatible
contains "uke" is chosen (fdtget needed), else the largest one.
"""
import struct, subprocess, sys, tempfile, os

def align(v, a): return (v + a - 1) // a * a

def main(src, dst):
    d = open(src, 'rb').read()
    if d[:8] != b'VNDRBOOT':
        sys.exit('not a vendor_boot image')
    ver, page, _, _, rsize = struct.unpack_from('<5I', d, 8)
    if ver < 3:
        sys.exit(f'unsupported vendor_boot header v{ver}')
    hdr_size, dtb_size = struct.unpack_from('<II', d, 2096)
    off = align(hdr_size, page) + align(rsize, page)
    blob = d[off:off + dtb_size]
    if len(blob) != dtb_size or dtb_size == 0:
        sys.exit('dtb region out of range')
    dtbs, p = [], 0
    while p + 8 <= len(blob) and blob[p:p+4] == b'\xd0\x0d\xfe\xed':
        total = struct.unpack_from('>I', blob, p + 4)[0]
        dtbs.append(blob[p:p+total]); p += total
    if not dtbs:
        sys.exit('no DTB magic found')
    print(f'{len(dtbs)} dtb(s) in vendor_boot: sizes {[len(x) for x in dtbs]}')
    pick = None
    for i, t in enumerate(dtbs):
        with tempfile.NamedTemporaryFile(suffix='.dtb', delete=False) as f:
            f.write(t); name = f.name
        try:
            comp = subprocess.run(['fdtget', '-t', 's', name, '/', 'compatible'],
                                  capture_output=True, text=True).stdout
        except FileNotFoundError:
            comp = ''
        os.unlink(name)
        print(f'  [{i}] {len(t)} B compatible: {comp.strip()}')
        if 'uke' in comp and pick is None:
            pick = t
    if pick is None:
        pick = max(dtbs, key=len)
        print('  no "uke" match: using the largest DTB')
    open(dst, 'wb').write(pick)
    print(f'wrote {dst} ({len(pick)} bytes)')

if len(sys.argv) != 3:
    sys.exit(__doc__)
main(*sys.argv[1:])
