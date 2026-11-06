#!/usr/bin/env python3
"""Compose and flash the full-macOS userdata root without writing it to disk first.

Layout (what NativeUfs reads, as prepare_macos26_ufs_root.py writes it):
  0          SHNGROOT manifest (4 KiB)
  1 MiB      window "OS":  the full system container (raw APFS) wrapped in a GPT (1 MiB prefix, 1 MiB tail)
  next MiB   window "ISC": the GPT-wrapped ISC seed, isc-window.img (pulled from the tablet; sha in full-root.json)
Usage:
  macos-full-root.py plan     -> prints windows, sizes and SHA-256 (the values for config/macos26-recovery-root.json)
  macos-full-root.py flash    -> streams the image to fastboot userdata in 512 MiB sparse pieces
"""
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

D = '/Users/daniel/Desktop/Deliver/Domotica'
OS_IMAGE = D + '/ipsw/macos26/fullos/25F71__MacOS/094-56453-087.dmg'
ISC_IMAGE = D + '/ipsw/macos26/fullos/isc-window.img'
STATE = D + '/ipsw/macos26/fullos/full-root.json'
MIB = 1 << 20
APFS_TYPE = uuid.UUID('7c3457ef-0000-11aa-aa11-00306543ecac')


def gpt_wrap(size, name):
    """Prefix and tail around a raw APFS container of `size` bytes (512-byte sectors, one APFS partition)."""
    start, total = MIB, size + 2 * MIB
    sectors = total // 512
    entries = bytearray(16384)
    ident = lambda tag: uuid.uuid5(uuid.NAMESPACE_URL, 'uke://full-macos/' + name + '/' + tag).bytes_le
    struct.pack_into('<16s16sQQQ72s', entries, 0, APFS_TYPE.bytes_le, ident('partition'),
                     start // 512, (start + size) // 512 - 1, 0, name.encode('utf-16le'))
    crc = zlib.crc32(entries)

    def header(current, other, table):
        block = bytearray(512)
        struct.pack_into('<8sIIIIQQQQ16sQIII', block, 0, b'EFI PART', 0x10000, 92, 0, 0, current, other,
                         34, sectors - 34, ident('disk'), table, 128, 128, crc)
        struct.pack_into('<I', block, 16, zlib.crc32(block[:92]))
        return bytes(block)

    prefix = bytearray(start)
    struct.pack_into('<B3sB3sII', prefix, 446, 0, b'\x00\x02\x00', 0xee, b'\xff\xff\xff', 1, sectors - 1)
    prefix[510:512] = b'\x55\xaa'
    prefix[512:1024] = header(1, sectors - 1, 2)
    prefix[1024:1024 + len(entries)] = entries
    tail = bytearray(MIB)
    tail[-512 - len(entries):-512] = entries
    tail[-512:] = header(sectors - 1, 1, sectors - 33)
    return bytes(prefix), bytes(tail)


def recovery_isc():
    """(offset, bytes, sha256) of the ISC window: the whole of isc-window.img, checked against full-root.json."""
    digest = json.load(open(STATE))['digests']['ISC']
    return 0, os.path.getsize(ISC_IMAGE), digest


def segments():
    """The composed image as (kind, source, offset_in_source, length) pieces, plus the window table."""
    os_size = os.path.getsize(OS_IMAGE)
    assert os_size % MIB == 0
    prefix, tail = gpt_wrap(os_size, 'uke macOS')
    isc_off, isc_size, _ = recovery_isc()
    windows = [dict(name='OS', offset=MIB, bytes=os_size + 2 * MIB),
               dict(name='ISC', offset=0, bytes=isc_size)]
    windows[1]['offset'] = (windows[0]['offset'] + windows[0]['bytes'] + MIB - 1) // MIB * MIB
    total = (windows[1]['offset'] + isc_size + MIB - 1) // MIB * MIB
    parts = [('mem', prefix, 0, len(prefix)), ('file', OS_IMAGE, 0, os_size), ('mem', tail, 0, len(tail)),
             ('zero', None, 0, windows[1]['offset'] - (windows[0]['offset'] + windows[0]['bytes'])),
             ('file', ISC_IMAGE, isc_off, isc_size),
             ('zero', None, 0, total - windows[1]['offset'] - isc_size)]
    return parts, windows, total


def stream(parts, chunk=8 * MIB):
    for kind, src, off, length in parts:
        if kind == 'mem':
            yield src[off:off + length]
        elif kind == 'zero':
            while length:
                n = min(chunk, length)
                yield bytes(n)
                length -= n
        else:
            with open(src, 'rb') as f:
                f.seek(off)
                while length:
                    data = f.read(min(chunk, length))
                    assert data, 'short read from ' + src
                    yield data
                    length -= len(data)


def manifest(windows, total, digests):
    header = bytearray(4096)
    header[:8] = b'SHNGROOT'
    struct.pack_into('<IIQ', header, 8, 1, len(windows), total)
    for i, w in enumerate(windows):
        struct.pack_into('<32sQQII32s40s', header, 24 + i * 128, w['name'].encode(), w['offset'], w['bytes'],
                         512, 1, bytes.fromhex(digests[w['name']]), bytes(40))
    header[4064:] = hashlib.sha256(header[:4064]).digest()
    return bytes(header)


def plan():
    parts, windows, total = segments()
    os_hash = hashlib.sha256()
    for data in stream(parts[:3]):
        os_hash.update(data)
    digests = {'OS': os_hash.hexdigest(), 'ISC': recovery_isc()[2]}
    state = dict(total=total, windows=windows, digests=digests, os_image_bytes=os.path.getsize(OS_IMAGE))
    open(STATE, 'w').write(json.dumps(state, indent=2) + '\n')
    print(json.dumps(state, indent=2))


def flash(partition='userdata', piece=512 * MIB):
    state = json.load(open(STATE))
    parts, windows, total = segments()
    assert total == state['total'] and windows == state['windows']
    head = manifest(windows, total, state['digests'])
    parts = [('mem', head, 0, 4096), ('zero', None, 0, MIB - 4096)] + parts
    block, written, buf = 4096, 0, bytearray()

    def send(data, first_block):
        count = len(data) // block
        chunks = []
        if first_block:
            chunks.append(struct.pack('<HHII', 0xCAC3, 0, first_block, 12))
        chunks.append(struct.pack('<HHII', 0xCAC1, 0, count, 12 + len(data)))
        hdr = struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, block, first_block + count, len(chunks), 0)
        with tempfile.NamedTemporaryFile(suffix='.simg', dir=os.path.dirname(STATE), delete=False) as t:
            t.write(hdr)
            if first_block:
                t.write(chunks[0])
            t.write(chunks[-1])
            t.write(data)
            name = t.name
        r = subprocess.run(['fastboot', 'flash', partition, name], capture_output=True, text=True)
        os.unlink(name)
        print(f'blocks {first_block}..{first_block + count}: rc={r.returncode}', flush=True)
        if r.returncode:
            print(r.stderr[-800:])
            sys.exit(1)

    for data in stream(parts):
        buf += data
        while len(buf) >= piece:
            send(bytes(buf[:piece]), written // block)
            written += piece
            del buf[:piece]
    if buf:
        buf += bytes(-len(buf) % block)
        send(bytes(buf), written // block)
        written += len(buf)
    print('DONE', written)


if __name__ == '__main__':
    {'plan': plan, 'flash': flash}[sys.argv[1]]()
