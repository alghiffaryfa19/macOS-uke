#!/usr/bin/env python3
"""The uke "DRV" window: a fixed 512 MiB area of userdata after the OS and ISC windows, for the GPU driver's userland
(Metal plugin, Mesa/Turnip, air2ir3) and the raw GPU firmware, rewritten with dd from OrangeFox instead of reflashing
the 25 GB system. NativeUfs publishes it as a read-only disk (forsheng/nativeufs-drv-window.patch); UkeGpu reads the
firmware table at drv.json "firmware_offset" through native_ufs_drv_read.

Layout of the window (drv.json):
  0 .. volume_bytes              GPT + APFS container with volume "UKEDRV" (from 1 MiB; the boot kernel has no HFS)
  firmware_offset (last 1 MiB)   table "UKEFW001" {u32 count, u32 0, {name[32], u64 offset, u64 bytes, sha256}[8]},
                                 then each image 4 KiB aligned; offsets are from the start of the window

Commands:
  image STAGING OUT      APFS volume from the folder STAGING plus the gen71100 firmware area -> OUT (512 MiB)
  manifest OUT [without-drv]
                         SHNGROOT header (4 KiB) for userdata with OS, ISC and DRV, from ipsw/macos26/fullos/full-root.json;
                         without-drv rebuilds today's two-window header (the rollback: an unpatched NativeUfs refuses three)
  trustcache BASE STAGING OUT
                         BASE (e.g. ipsw/macos26/fullos/full-os.trustcache) plus the cdhash of every Mach-O in STAGING
                         (ad-hoc signed beforehand), as one sorted static module -> OUT
  verify IMAGE           check the firmware table and hashes of a built image
"""
import hashlib
import json
import os
import runpy
import struct
import uuid
import zlib
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
D = HERE.parents[1]
DRV = json.loads((HERE / 'drv.json').read_text())
# Our iPadOS 27 unit's pins (UniXNU-uke-allenwdk, branch uke-a27-local: our own zap), as stage.py.
FIRMWARE = D / 'UniXNU-uke-allenwdk/.cache/firmware/uke-gpu'
INPUTS = D / 'UniXNU-uke-allenwdk/scripts/data/uke-gpu-inputs.json'
FULL_ROOT = D / 'ipsw/macos26/fullos/full-root.json'
MIB = 1 << 20
ENTRY = struct.Struct('<32sQQ32s')


def check_contract():
    if (DRV['window'] != 'DRV' or DRV['bytes'] % MIB or DRV['volume_bytes'] != DRV['firmware_offset']
            or DRV['firmware_offset'] + DRV['firmware_bytes'] != DRV['bytes'] or DRV['volume_bytes'] % MIB):
        sys.exit('drv.json is inconsistent')


def firmware_area():
    pins = json.loads(INPUTS.read_text())['firmware']
    blobs = []
    for role in ('gmu', 'sqe', 'zap'):
        data = (FIRMWARE / pins[role]['name']).read_bytes()
        if len(data) != pins[role]['bytes'] or hashlib.sha256(data).hexdigest() != pins[role]['sha256']:
            sys.exit('%s does not match its pin' % pins[role]['name'])
        blobs.append((role, data))
    area = bytearray(DRV['firmware_bytes'])
    cursor = 4096
    entries = b''
    for role, data in blobs:
        entries += ENTRY.pack(role.encode(), DRV['firmware_offset'] + cursor, len(data), hashlib.sha256(data).digest())
        area[cursor:cursor + len(data)] = data
        cursor += (len(data) + 4095) & ~4095
        if cursor > len(area):
            sys.exit('firmware does not fit the firmware area')
    area[:16 + len(entries)] = b'UKEFW001' + struct.pack('<II', len(blobs), 0) + entries
    return bytes(area)


APFS_TYPE = uuid.UUID('7c3457ef-0000-11aa-aa11-00306543ecac')


def gpt(container_bytes):
    """Protective MBR + GPT for the whole 512 MiB window, one APFS partition from 1 MiB over the container; the backup
    entries and header sit in the window's last 33 sectors, after the firmware images (the kernel only probes APFS
    inside a partition of the APFS type, as the OS and ISC windows are wrapped)."""
    sectors = DRV['bytes'] // 512
    first, last = MIB // 512, (MIB + container_bytes) // 512 - 1
    ident = lambda tag: uuid.uuid5(uuid.NAMESPACE_URL, 'uke://drv/' + tag).bytes_le
    entries = bytearray(16384)
    struct.pack_into('<16s16sQQQ72s', entries, 0, APFS_TYPE.bytes_le, ident('partition'), first, last, 0,
                     DRV['volume_name'].encode('utf-16le'))
    crc = zlib.crc32(entries)

    def header(current, other, table):
        block = bytearray(512)
        struct.pack_into('<8sIIIIQQQQ16sQIII', block, 0, b'EFI PART', 0x10000, 92, 0, 0, current, other,
                         34, sectors - 34, ident('disk'), table, 128, 128, crc)
        struct.pack_into('<I', block, 16, zlib.crc32(block[:92]))
        return bytes(block)

    head = bytearray(MIB)
    struct.pack_into('<B3sB3sII', head, 446, 0, b'\x00\x02\x00', 0xee, b'\xff\xff\xff', 1, sectors - 1)
    head[510:512] = b'\x55\xaa'
    head[512:1024] = header(1, sectors - 1, 2)
    head[1024:1024 + len(entries)] = entries
    tail = bytes(entries) + header(sectors - 1, 1, sectors - 33)
    return bytes(head), tail


def image(staging, out):
    """APFS volume UKEDRV (the boot kernel has APFS but no HFS) in a GPT, plus the firmware area."""
    check_contract()
    staging, out = Path(staging), Path(out)
    tmp = out.with_name(out.name + '.apfs')
    for leftover in (tmp, tmp.with_name(tmp.name + '.cdr')):
        leftover.unlink(missing_ok=True)
    container_bytes = DRV['volume_bytes'] - MIB
    subprocess.run(['hdiutil', 'create', '-quiet', '-srcfolder', str(staging), '-fs', 'APFS', '-volname',
                    DRV['volume_name'], '-layout', 'NONE', '-format', 'UDTO',
                    '-size', '%dm' % (container_bytes // MIB), str(tmp)], check=True)
    raw = tmp.with_name(tmp.name + '.cdr')
    size = raw.stat().st_size
    with open(raw, 'rb') as f:
        f.seek(32)
        magic = f.read(4)
    if size > container_bytes or magic != b'NXSB':
        raw.unlink()
        sys.exit('hdiutil made %d bytes (%r), not an APFS container within %d' % (size, magic, container_bytes))
    head, tail = gpt(container_bytes)
    with open(out, 'wb') as o:
        o.write(head)
        with open(raw, 'rb') as f:
            while chunk := f.read(8 * MIB):
                o.write(chunk)
        o.truncate(DRV['volume_bytes'])
        o.seek(DRV['volume_bytes'])
        o.write(firmware_area())
        o.seek(DRV['bytes'] - len(tail))
        o.write(tail)
    raw.unlink()
    verify(out)


def verify(path):
    with open(path, 'rb') as f:
        f.seek(0, 2)
        if f.tell() != DRV['bytes']:
            sys.exit('%s is %d bytes, not %d' % (path, f.tell(), DRV['bytes']))
        f.seek(512)
        if f.read(8) != b'EFI PART':
            sys.exit('no GPT header')
        f.seek(DRV['bytes'] - 512)
        if f.read(8) != b'EFI PART':
            sys.exit('no backup GPT header')
        f.seek(MIB + 32)
        if f.read(4) != b'NXSB':
            sys.exit('no APFS container at 1 MiB')
        f.seek(DRV['firmware_offset'])
        table = f.read(4096)
        if table[:8] != b'UKEFW001':
            sys.exit('no firmware table')
        count = struct.unpack_from('<I', table, 8)[0]
        last_end = 0
        for i in range(count):
            name, offset, size, digest = ENTRY.unpack_from(table, 16 + i * ENTRY.size)
            f.seek(offset)
            if hashlib.sha256(f.read(size)).digest() != digest:
                sys.exit('%s fails its SHA-256' % name.rstrip(b'\0').decode())
            last_end = max(last_end, offset + size)
            print('  %-4s offset %#x  %6d bytes  sha256 %s' % (name.rstrip(b'\0').decode(), offset, size, digest.hex()[:16]))
        if last_end > DRV['bytes'] - 33 * 512:
            sys.exit('firmware overlaps the backup GPT')
    print('%s: DRV image ok (%d bytes, GPT + APFS %s, %d firmware images)' % (path, DRV['bytes'], DRV['volume_name'], count))


def manifest(out, variant='with-drv'):
    check_contract()
    state = json.loads(FULL_ROOT.read_text())
    windows = [w for w in state['windows'] if w['name'] != 'DRV']
    end = max(w['offset'] + w['bytes'] for w in windows)
    offset = (end + MIB - 1) // MIB * MIB
    total = state['total']
    digests = dict(state['digests'])
    if variant == 'with-drv':
        windows.append(dict(name='DRV', offset=offset, bytes=DRV['bytes']))
        total = offset + DRV['bytes']
        digests['DRV'] = '00' * 32   # DRV is rewritten in place: never digest-checked
    elif variant != 'without-drv':
        sys.exit('manifest variant is with-drv or without-drv')
    composer = runpy.run_path(str(D / 'boot/macos-full-root.py'), run_name='composer')
    header = composer['manifest'](windows, total, digests)
    Path(out).write_bytes(header)
    print(json.dumps(dict(total=total, windows=windows), indent=2))
    print('manifest %s sha256 %s' % (out, hashlib.sha256(header).hexdigest()))
    print('OrangeFox (userdata = /dev/block/by-name/userdata):')
    print('  dd if=/tmp/%s of=/dev/block/by-name/userdata bs=4096 count=1 conv=notrunc,fsync' % Path(out).name)
    if variant == 'with-drv':
        print('  dd if=/tmp/drv.img of=/dev/block/by-name/userdata bs=1048576 seek=%d conv=notrunc,fsync' % (offset // MIB))


def cdhash(path):
    text = subprocess.run(['codesign', '-dvvv', str(path)], capture_output=True, text=True).stderr
    hashes = [line.split('=', 1)[1] for line in text.splitlines() if line.startswith('CDHash=')]
    kinds = [line for line in text.splitlines() if line.startswith('Hash type=')]
    if len(hashes) != 1 or not kinds or 'sha256' not in kinds[0]:
        sys.exit('%s: needs one SHA-256 code directory (codesign -s - first)\n%s' % (path, text))
    return bytes.fromhex(hashes[0])[:20]


def macho(path):
    with open(path, 'rb') as f:
        return f.read(4) in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe')


def trustcache(base, staging, out):
    data = Path(base).read_bytes()
    count, at = struct.unpack_from('<II', data, 0)
    if count != 1 or at != 8:
        sys.exit('expected one static module at offset 8')
    version, uuid_bytes, n = struct.unpack_from('<I16sI', data, at)
    if version != 2:
        sys.exit('expected a version 2 module')
    entries = {data[at + 24 + 24 * i:at + 44 + 24 * i]: data[at + 24 + 24 * i:at + 48 + 24 * i] for i in range(n)}
    added = []
    for path in sorted(Path(staging).rglob('*')):
        if path.is_file() and not path.is_symlink() and macho(path):
            digest = cdhash(path)
            if digest not in entries:
                entries[digest] = digest + bytes([2, 0, 0, 0])   # SHA-256 truncated, no flags, no launch constraint
                added.append((path.relative_to(staging), digest.hex()))
    module = struct.pack('<I16sI', 2, uuid_bytes, len(entries)) + b''.join(entries[k] for k in sorted(entries))
    Path(out).write_bytes(struct.pack('<II', 1, 8) + module)
    for name, digest in added:
        print('  + %s %s' % (digest, name))
    print('%s: %d entries (%d added)' % (out, len(entries), len(added)))


if __name__ == '__main__':
    commands = {'image': image, 'manifest': manifest, 'trustcache': trustcache, 'verify': verify}
    if len(sys.argv) < 2 or sys.argv[1] not in commands:
        sys.exit(__doc__)
    commands[sys.argv[1]](*sys.argv[2:])
