#!/usr/bin/env python3
"""Compile the patched ArmDTPlatform/ArmCpu with xnu's own IOPlatformExpert compile line,
as macOS-ForSheng's build_macos26_xnu.py does."""
import json
import subprocess
import sys
from pathlib import Path

OUT = Path(sys.argv[1])
SRC = OUT / 'source'
WARN = ('unused-but-set-variable', 'gnu-folding-constant', 'implicit-int-float-conversion',
        'pointer-to-int-cast', 'void-pointer-to-int-cast', 'void-pointer-to-enum-cast')
record = json.loads((OUT / 'obj/DEVELOPMENT_ARM64_SHENG/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                    .read_text().strip().rstrip(','))
for component in ('ArmDTPlatform', 'ArmCpu'):
    adapted = OUT / 'machine' / component / (component + '.cpp')
    dest = OUT / (component + '.o')
    cmd, args, i = [], record['arguments'], 0
    while i < len(args):
        a = args[i]
        if a == record['file']:
            cmd.append(str(adapted))
        elif a in ('-o', '-MF', '-MT', '-MQ', '-MJ'):
            if a == '-o':
                cmd += ['-o', str(dest)]
            i += 1
        elif a not in ('-MMD', '-MD', '-MP'):
            cmd.append(a)
        i += 1
    cmd += ['-I', str(SRC / 'osfmk'), '-Werror', '-Wno-unused-parameter', '-Wno-poison-system-directories'] + ['-Werror=' + w for w in WARN]
    r = subprocess.run(cmd, cwd=record['directory'], capture_output=True, text=True)
    print(component, 'ok' if r.returncode == 0 else 'FAILED')
    if r.returncode:
        print(r.stderr[-4000:])
        sys.exit(1)
