#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile the WiFi board integration and archive it with the Ath12kWifi component.

This script is a kernel archive builder listed in board.toml [kernel] archives.
It compiles WifiProbe.cpp and WifiNet.cpp (from the kit's package/) against the
XNU source-kernel ABI, then merges them with the pre-built Ath12kWifi component
library into a single static archive for the kernel prelink.

The Ath12kWifi component (drivers/Ath12kWifi/) provides:
  - ath12k PCI transport (nw_pci.c)
  - MHI host interface (nw_mhi.c)
  - QRTR/QMI messaging (nw_qrtr.c, nw_qrtr_mhi.c)
  - mac80211 station management (nw_mac80211.c, nw_sta.c)
  - WPA2/WPA3 crypto (nw_crypto.c)
  - 802.11 frame handling (nw_wifi.c, nw_net.c)

The board provides (via WifiProbe.cpp + WifiNet.cpp):
  - PCIe link training and endpoint discovery
  - PMIC rail votes for the WCN7850 PMU
  - SMMU stream mapping for the endpoint's DMA
  - nw_board / lx_platform structs
  - ifnet interface, Apple80211 ioctl handling, IOKit registry
"""
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

BOARD_SOURCES = ['WifiProbe.cpp', 'WifiNet.cpp']


def compile_one(source: Path, destination: Path, record: dict, extra_includes: list[Path],
                extra_defines: list[str] | None = None) -> None:
    """Compile a single C++ source using the XNU compile record."""
    arguments = record['arguments']
    command = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(destination)])
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    command.extend(['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-Wno-c99-designator', '-Wno-reorder-init-list',
                    '-Wno-deprecated-enum-enum-conversion',
                    '-Wno-unnecessary-virtual-specifier',
                    '-I', str(ROOT / '.cache/xnu-work/osfmk')])
    for inc in extra_includes:
        command.extend(['-I', str(inc)])
    for d in (extra_defines or []):
        command.append(d)
    result = subprocess.run(command, cwd=record['directory'],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise SystemExit(f'{source.name}: compile failed\n{result.stdout[-3500:]}')
    verify_object(destination, source.name)


def verify_object(path: Path, label: str) -> None:
    """Check that the object is ARM64 iOS 17."""
    data = path.read_bytes()
    magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit(f'{label}: unexpected Mach-O header')
    offset = 32
    deployment = None
    for _ in range(count):
        cmd, length = struct.unpack_from('<II', data, offset)
        if cmd == 0x32:
            deployment = struct.unpack_from('<II', data, offset + 8)
        offset += length
    if deployment != (2, 0x110000):
        raise SystemExit(f'{label}: unexpected deployment target {deployment}')


def main() -> int:
    recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    if not recipe.exists():
        raise SystemExit(f'Compile record not found: {recipe}\n'
                         'Run a bootstrap kernel build first (CONFIG_XNU_BUILD_ARCHIVES=y).')
    record = json.loads(recipe.read_text().strip().rstrip(','))

    out = ROOT / 'build/ath12k-wifi'
    out.mkdir(parents=True, exist_ok=True)

    kit_dir = ROOT / 'kits' / 'uke'
    if not kit_dir.exists():
        kit_dir = ROOT / 'package'

    wifi_driver = ROOT / 'drivers' / 'Ath12kWifi'
    extra_includes = [
        wifi_driver / 'include',
        kit_dir,
    ]

    objects = []
    for name in BOARD_SOURCES:
        source = kit_dir / name
        if not source.exists():
            raise SystemExit(f'Board source not found: {source}')
        obj = out / name.replace('.cpp', '.o')
        print(f'  compile {name}')
        compile_one(source, obj, record, extra_includes)
        objects.append(obj)

    component_lib = wifi_driver / 'lib' / 'libAth12kWifi.a'
    if not component_lib.exists():
        component_lib = ROOT / 'build' / 'ath12k' / 'libAth12kWifi.a'

    archive = out / 'libAth12kWifi.a'
    archive.unlink(missing_ok=True)

    if component_lib.exists():
        # Merge board objects with the pre-built component library:
        # extract component objects into a temp dir, then archive everything together.
        merge_dir = out / '_merge'
        merge_dir.mkdir(exist_ok=True)
        subprocess.run(['xcrun', 'ar', 'x', str(component_lib)],
                       cwd=str(merge_dir), check=True)
        component_objects = list(merge_dir.glob('*.o'))
        all_objects = [str(o) for o in objects] + [str(o) for o in component_objects]
        subprocess.run(['xcrun', 'ar', 'rcs', str(archive)] + all_objects, check=True)
        for o in component_objects:
            o.unlink()
        merge_dir.rmdir()
    else:
        print(f'  warning: component library not found at {component_lib}')
        print(f'  archiving board objects only (link will need the component at prelink time)')
        subprocess.run(['xcrun', 'ar', 'rcs', str(archive)] + [str(o) for o in objects],
                       check=True)

    (out / 'compile-command.json').write_text(
        json.dumps({'board_sources': BOARD_SOURCES, 'archive': str(archive),
                    'component_lib': str(component_lib) if component_lib.exists() else None},
                   indent=2) + '\n')

    print(f'Ath12kWifi: -Werror build and ARM64/iOS 17 object audit passed → {archive}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
