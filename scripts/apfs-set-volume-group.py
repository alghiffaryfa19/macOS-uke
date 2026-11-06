#!/usr/bin/env python3
"""Put two APFS volumes of a raw container image into one volume group (what `diskutil apfs addVolume -groupWith`
does, which refuses on the unsealed IPSW system volume). Every on-disk copy of each volume superblock (APSB) gets
apfs_volume_group_id = GROUP, then its Fletcher-64 object checksum is recomputed.
Usage: apfs-set-volume-group.py IMAGE SYSTEM_VOL_UUID DATA_VOL_UUID [GROUP_UUID]
"""
import struct
import sys
import uuid

BLOCK = 4096
OFF_VOL_UUID, OFF_ROLE, OFF_GROUP = 0xf0, 0x3c4, 0x3f0
ROLE_SYSTEM, ROLE_DATA = 1, 0x40


def fletcher64(data):
    lo = hi = 0
    for (word,) in struct.iter_unpack('<I', data):
        lo = (lo + word) % 0xffffffff
        hi = (hi + lo) % 0xffffffff
    c0 = 0xffffffff - ((lo + hi) % 0xffffffff)
    c1 = 0xffffffff - ((lo + c0) % 0xffffffff)
    return struct.pack('<II', c0, c1)


image, system, data = sys.argv[1], uuid.UUID(sys.argv[2]), uuid.UUID(sys.argv[3])
group = uuid.UUID(sys.argv[4]) if len(sys.argv) > 4 else uuid.uuid4()
want = {system.bytes: ROLE_SYSTEM, data.bytes: ROLE_DATA}
patched = {ROLE_SYSTEM: 0, ROLE_DATA: 0}
with open(image, 'r+b') as f:
    offset = 0
    while True:
        block = f.read(BLOCK)
        if len(block) < BLOCK:
            break
        if block[32:36] == b'APSB' and block[OFF_VOL_UUID:OFF_VOL_UUID + 16] in want:
            role = struct.unpack_from('<H', block, OFF_ROLE)[0]
            if role != want[block[OFF_VOL_UUID:OFF_VOL_UUID + 16]]:
                sys.exit(f'unexpected role {role:#x} at {offset:#x}: check the superblock layout')
            if fletcher64(block[8:]) != block[:8]:
                sys.exit(f'bad checksum at {offset:#x} before patching')
            b = bytearray(block)
            b[OFF_GROUP:OFF_GROUP + 16] = group.bytes
            b[:8] = fletcher64(bytes(b[8:]))
            f.seek(offset)
            f.write(b)
            patched[role] += 1
        offset += BLOCK
print(f'group {str(group).upper()}: system copies {patched[ROLE_SYSTEM]}, data copies {patched[ROLE_DATA]}')
