#!/usr/bin/env python3
"""uke macOS: the Mac-primed cache directory (run-prime.sh) as the bundle's pipeline pack, NativeAdrenoMetal.m2vpack.

Format (drivers/AdrenoMetal/CacheExport.h, the same scripts/make-m2vpack.py writes): b"SHEXP001", u32 stream bytes, u32 compressed bytes,
then the zlib-compressed stream b"M2VCACH1", u32 root length, root, u32 file count, per file u32 path length, u32 mode, u32 uid, u32 gid,
u64 size, path, contents. WindowServer imports it into its cache directory before the device opens (NativeAdrenoMetalPlugin.mm,
cacheexport::importPack), so the first pipelines of every boot are cache hits and priming finds every library done.

The priming records (prime-done, prime-fn, prime-bad, prime.log) name each library by its path on this Mac's mount; on the tablet the
same files sit at the same path without the mount prefix, with the same size and mtime (the same sealed system volume), so the prefix is
dropped. Nothing else in the directory names a path (checked: the translator's records are content-addressed). Mesa's own disk cache
(ir3/) holds only its index and is left out, as are temporary files.
usage: make-pack.py <cache dir> <mount prefix> <out.m2vpack>
"""
import os
import struct
import sys
import zlib
from pathlib import Path

root, prefix, out = Path(sys.argv[1]), sys.argv[2].rstrip('/'), Path(sys.argv[3])
RECORDS = {'prime-done', 'prime-fn', 'prime-bad', 'prime.log'}
files = []
for path in sorted(root.rglob('*')):
    if not path.is_file():
        continue
    rel = path.relative_to(root).as_posix()
    if rel.startswith('ir3/') or rel == 'prime-current' or path.name.startswith('.') or rel.endswith('.tmp') or '.import.' in rel:
        continue
    data = path.read_bytes()
    if rel in RECORDS:
        data = data.replace(prefix.encode() + b'/System/', b'/System/')
        if prefix.encode() in data:
            sys.exit(f'{rel}: still names {prefix}')
    files.append((rel, 0o600, data))
stream = bytearray(b'M2VCACH1' + struct.pack('<I', len(str(root))) + str(root).encode() + struct.pack('<I', len(files)))
for rel, mode, data in files:
    name = rel.encode()
    stream += struct.pack('<IIIIQ', len(name), mode, 0, 0, len(data)) + name + data
z = zlib.compress(bytes(stream), 6)
if len(stream) >= 1 << 32 or len(z) + 16 > 1 << 30:
    sys.exit('pack too large for the importer')
out.write_bytes(b'SHEXP001' + struct.pack('<II', len(stream), len(z)) + z)
print(f'{out}: {len(files)} files, {len(stream) >> 20} MiB stream, {len(z) >> 20} MiB compressed')
