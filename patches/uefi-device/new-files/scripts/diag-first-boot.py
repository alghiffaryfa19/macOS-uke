#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""First-boot diagnostic toolkit for the uke (SM7675) UniXNU port.

Run this on the host Mac after a boot attempt to parse the PRAM/ramoops
panic log, analyze GPU memory reservation, and generate a hardware
verification checklist. It consolidates what PROGRESS.md identified as
the root cause chain:

  SMP + display stack → GPU GMU stage 4 memory failure
  → kernel freeze → Gunyah hypervisor watchdog bite → reboot

Usage:
  python3 diag-first-boot.py --pram <path-to-pram-dump>
  python3 diag-first-boot.py --checklist
  python3 diag-first-boot.py --gpu-memory
"""
import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


# ---- PRAM / Ramoops panic log parser ----

def parse_pram(path: Path) -> None:
    """Parse a binary pram dump or text ramoops log."""
    data = path.read_bytes()

    # Try to find XNU panic string (starts with "panic(cpu" or "Debugger message:")
    text = None
    for encoding in ('utf-8', 'ascii', 'latin-1'):
        try:
            decoded = data.decode(encoding, errors='replace')
            # Look for panic signature
            for marker in ('panic(cpu', 'Debugger message:', 'kernel trap at',
                           'watchdog timeout', 'WDT bite', 'Gunyah'):
                idx = decoded.find(marker)
                if idx >= 0:
                    start = max(0, idx - 200)
                    end = min(len(decoded), idx + 2000)
                    text = decoded[start:end]
                    break
            if text:
                break
        except Exception:
            continue

    if not text:
        # Try ramoops-style: search for console- or pmsg- prefixes
        for prefix in (b'console-ramoops', b'pmsg-ramoops', b'dmesg-ramoops'):
            idx = data.find(prefix)
            if idx >= 0:
                text = data[idx:idx + 4096].decode('utf-8', errors='replace')
                break

    if not text:
        print(f'No panic signature found in {path} ({len(data)} bytes)')
        print('Tip: on Gunyah watchdog resets, the panic log may be in:')
        print('  /sys/fs/pstore/console-ramoops-0  (Linux ramoops)')
        print('  PRAM partition (UFS LUN 0, check board.toml for offset)')
        print('  IMEM/QDSS (requires Qualcomm tools)')
        return

    print(f'=== Panic log from {path.name} ({len(data)} bytes) ===\n')
    print(text[:3000])

    # Extract key diagnostics
    if 'vm_page_find_contiguous' in text:
        print('\n*** GPU MEMORY: vm_page_find_contiguous failure detected ***')
        print('This confirms the GPU GMU stage 4 memory allocation issue.')
        print('Fix: add reserved-memory node in ADT for GPU contiguous region.')
        m = re.search(r'scanned (\d+).*?wired.*?(\d+)', text)
        if m:
            print(f'  Pages scanned: {m.group(1)}, Wired: {m.group(2)}')

    if 'watchdog' in text.lower() or 'wdt' in text.lower() or 'gunyah' in text.lower():
        print('\n*** WATCHDOG: Gunyah hypervisor watchdog bite ***')
        print('The kernel stopped petting the watchdog, likely due to:')
        print('  1. SMP freeze (display stack dependency)')
        print('  2. GPU GMU initialization hang')
        print('Check: was uke-smp=1 in kernel args? Try uke-smp=0 first.')

    cpus = re.findall(r'cpu (\d+)', text)
    if cpus:
        print(f'\n  CPUs mentioned in panic: {", ".join(sorted(set(cpus)))}')

    if 'freeze' in text.lower() or 'hung' in text.lower():
        print('\n*** FREEZE: kernel freeze or hang detected ***')
        print('Likely the SMP + display code path. Timeline from PROGRESS.md:')
        print('  - Single CPU: stable 877+ seconds')
        print('  - SMP: freezes at ~80-96 seconds')


# ---- GPU memory reservation analysis ----

def analyze_gpu_memory() -> None:
    """Analyze the GPU memory reservation requirements."""
    print('=== GPU Memory Reservation Analysis ===\n')
    print('Root cause: GPU GMU cold-start calls vm_page_find_contiguous(),')
    print('which fails because XNU cannot find a large enough physically')
    print('contiguous region among free pages (scanned ~47503, wired ~34485).\n')

    print('Fix path (from PROGRESS.md and sheng reference):')
    print('  1. Add reserved-memory node in the ADT (make-uke-adt.py)')
    print('  2. The GPU driver must use IODeviceMemory::withRange() to adopt')
    print('     the pre-reserved region instead of vm_page_find_contiguous()')
    print('  3. This is the same pattern used on sheng (SM8550)\n')

    # Check current ADT script
    adt_script = ROOT / 'scripts' / 'make-uke-adt.py'
    if adt_script.exists():
        text = adt_script.read_text()
        has_gpu_reserved = 'gpu' in text.lower() and 'reserved' in text.lower()
        print(f'ADT script: {adt_script}')
        print(f'  GPU reserved-memory node: {"PRESENT" if has_gpu_reserved else "MISSING — needs to be added"}')
    else:
        print(f'ADT script not found at {adt_script}')

    # Check board.toml GPU section
    board_toml = ROOT / 'board.toml'
    if board_toml.exists():
        text = board_toml.read_text()
        if 'gmem_bytes' in text:
            m = re.search(r'gmem_bytes\s*=\s*"(0x[0-9a-fA-F]+)"', text)
            if m:
                gmem = int(m.group(1), 16)
                print(f'\n  GPU GMEM size from board.toml: {m.group(1)} ({gmem // 1024} KiB)')

    print('\nReserved memory node template for ADT:')
    print('  path: /reserved-memory/gpu_mem')
    print('  reg: <base> <size>  (size >= gmem_bytes + GMU working buffers)')
    print('  no-map;')
    print('  compatible = "qcom,adreno-smmu-guard";')
    print('\nThe base address must be within the DRAM range the XNU kernel')
    print('owns (dram_base in board.toml payload section).')
    print('Suggested: carve 8 MiB from the top of owned_memory_size.')

    # Check if preempt_record_bytes is configured
    if board_toml.exists():
        text = board_toml.read_text()
        m = re.search(r'preempt_record_bytes\s*=\s*"(0x[0-9a-fA-F]+)"', text)
        if m:
            preempt = int(m.group(1), 16)
            gmem_val = 0x180000  # from board.toml
            total = gmem_val + preempt + 0x200000  # + 2 MiB for GMU buffers
            print(f'\n  Estimated reservation: GMEM ({gmem_val:#x}) + preempt ({preempt:#x}) + GMU buffers (0x200000)')
            print(f'  Total: {total:#x} ({total // (1024*1024)} MiB)')


# ---- Hardware verification checklist ----

def print_checklist() -> None:
    """Print the first-boot hardware verification checklist."""
    print('=== Uke (SM7675) First-Boot Hardware Verification Checklist ===\n')

    checks = [
        ('BOOT STAGE', [
            'UEFI reaches UniXNU menu (Mu-Silicium stage)',
            'DarwinBoot loads XNU kernel',
            'XNU prints banner to framebuffer',
            'Apple logo appears (Stage 7 — current milestone)',
            'Post-logo: record exact time-to-reboot with stopwatch',
        ]),
        ('SINGLE CPU vs SMP', [
            'Boot with uke-smp=0 — record uptime at reboot',
            'Boot with uke-smp=1 — record uptime at reboot',
            'Expected: single-CPU stable 877+ sec, SMP freezes ~80-96 sec',
            'If single-CPU also reboots quickly: new blocker (not SMP)',
        ]),
        ('DEVICE TREE DUMP', [
            'From rooted Android: cat /proc/device-tree/... or dtc dump',
            'Record TLMM base (expect 0xf000000)',
            'Record GCC base (expect 0x100000)',
            'Record QUP wrapper addresses and SE assignments',
            'Record PCIe0 reg addresses and ranges',
            'Record WLAN GPIO numbers (WLAN_EN, PERST#, SW_CTRL)',
            'Record PCIe0 SMMU stream IDs',
            'Record WiFi chip PCI vendor:device ID from lspci if available',
        ]),
        ('PRAM / PANIC LOG', [
            'After reboot, read PRAM partition from Android recovery',
            'Look for: panic(cpu, watchdog, vm_page_find_contiguous',
            'Run: python3 scripts/diag-first-boot.py --pram <dump>',
        ]),
        ('GPU (Critical Path)', [
            'Check GPU firmware files exist on vendor partition:',
            '  gen71100_sqe.fw, gen71100_gmu.bin, gen71100_zap.mbn',
            'Run: python3 scripts/diag-first-boot.py --gpu-memory',
            'If vm_page_find_contiguous fails: add reserved-memory to ADT',
            'Test with offline-mobile-graphics-load=0 to isolate GPU from boot',
        ]),
        ('WIFI (WCN7850 on PCIe)', [
            'Boot with uke-wifi=1 to run Stage 1 probe (read-only)',
            'Read native_wifi_stats[] from pram or boot log:',
            '  stats[0]=1 means Stage 1 reached',
            '  stats[3] = GDSC state, stats[4] = clock halt bits',
            '  stats[5] = GPIO state (WLAN_EN | PERST# << 16)',
            '  stats[6] = PARF LTSSM state',
            'Compare GDSC/clock/GPIO with expected values from gcc-cliffs.c',
            'If all good: advance to uke-wifi=2 (link training)',
        ]),
        ('TOUCH (NT36532 on SPI)', [
            'Verify SPI SE4 GPIO pins from DT dump (currently placeholder)',
            'Check GPIO 54 interrupt routing',
            'Check QUP wrapper 0 halt registers match TouchBoard_uke.h',
        ]),
        ('UFS', [
            'Confirm UFS controller at 0x1d84000',
            'Confirm UFS PHY at 0x1d80000',
            'Check userdata partition GPT matches board.toml first_lba/last_lba',
        ]),
    ]

    for section, items in checks:
        print(f'[ ] {section}')
        for item in items:
            print(f'    [ ] {item}')
        print()

    print('Priority order: Boot Stage → Single/SMP → DT Dump → PRAM → GPU → WiFi')
    print('The DT dump unblocks everything else — get it first.\n')
    print('Quick wins (no hardware needed but not done yet):')
    print('  - Add reserved-memory/gpu_mem node to make-uke-adt.py')
    print('  - Prepare ADT nodes for GPU GMU memory (IODeviceMemory path)')
    print('  - Implement WifiProbe stages 2-5 with TODO(hardware) values')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--pram', type=Path, help='Path to PRAM/ramoops dump file')
    parser.add_argument('--checklist', action='store_true', help='Print hardware verification checklist')
    parser.add_argument('--gpu-memory', action='store_true', help='Analyze GPU memory reservation')
    args = parser.parse_args()

    if not any([args.pram, args.checklist, args.gpu_memory]):
        print_checklist()
        print('---')
        analyze_gpu_memory()
        return 0

    if args.pram:
        parse_pram(args.pram)
    if args.checklist:
        print_checklist()
    if args.gpu_memory:
        analyze_gpu_memory()

    return 0


if __name__ == '__main__':
    sys.exit(main())
