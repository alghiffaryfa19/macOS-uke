#!/usr/bin/env python3
"""Write the corecrypto POST HMAC into the linked macOS kernel (as macOS-ForSheng's seal step):
HMAC-SHA256 with a zero 32-byte key over __cc26_text, stored in the 32-byte __fips_hmacs field."""
import hashlib
import hmac
import shutil
import struct
import sys
from pathlib import Path

src, dst = Path(sys.argv[1]), Path(sys.argv[2])
data = bytearray(src.read_bytes())
assert struct.unpack_from('<4I', data)[:2] == (0xfeedfacf, 0x100000c)
cursor, code, field, base = 32, None, None, None
for _ in range(struct.unpack_from('<I', data, 16)[0]):
    kind, length = struct.unpack_from('<II', data, cursor)
    if kind == 0x19:
        segname, va = struct.unpack_from('<16sQ', data, cursor + 8)
        if segname.rstrip(b'\0') == b'__TEXT':
            base = va
        for i in range(struct.unpack_from('<I', data, cursor + 64)[0]):
            s = cursor + 72 + i * 80
            name, seg, addr, size, off = struct.unpack_from('<16s16sQQI', data, s)
            if name.rstrip(b'\0') == b'__cc26_text' and code is None:
                code = off, size, addr
            if name.rstrip(b'\0') == b'__fips_hmacs':
                assert size == 32 and field is None
                field = off
    cursor += length
assert code and field is not None and base is not None and code[2] - base == code[0]
mac = hmac.digest(bytes(32), data[code[0]:code[0] + code[1]], 'sha256')
data[field:field + 32] = mac
dst.write_bytes(data)
print('sealed', dst, 'text', code[1], 'bytes, hmac', mac.hex()[:16])
