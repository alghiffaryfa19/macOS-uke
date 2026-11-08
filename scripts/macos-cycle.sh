#!/bin/bash
# One test cycle of the macOS port on uke, without hand-typed steps.
#   boot/macos-cycle.sh pull M2y          tablet in fastboot (or OrangeFox): ROM boot -> OrangeFox, read + decode macoslog, summary
#   boot/macos-cycle.sh flash <image>     from OrangeFox or fastboot: boot_a = UniXNU UEFI AND vendor_boot_a = image, slot a, reboot
#   boot/macos-cycle.sh guard 512k|64k    from OrangeFox: macoslog guard block for the 512 KiB (kernel >= m34) or 64 KiB log
#   boot/macos-cycle.sh summary M2y       summary of an already pulled log
# Never flashes userdata. OrangeFox is reached by the ROM boot/vendor_boot (fastboot boot of OrangeFox fails: Bad Buffer Size).
set -euo pipefail
D=/Users/daniel/Desktop/Deliver/Domotica
L=$D/boot/macoslog
ROM=~/Desktop/woa-uke/device_dumps/backup-pre-windows
GUARD_LBA=30693903
LOG_LBA=30693392

wait_fastboot() { for _ in $(seq 1 60); do fastboot devices | grep -q fastboot && return 0; sleep 2; done; echo "no fastboot"; exit 1; }
wait_ofox() { for _ in $(seq 1 60); do adb devices | grep -q recovery && return 0; sleep 2; done; echo "no OrangeFox"; exit 1; }
in_ofox() { adb devices | grep -q recovery; }

to_ofox() {
  in_ofox && return 0
  wait_fastboot
  fastboot flash boot_a $ROM/boot_a.img | tail -1
  fastboot flash vendor_boot_a $ROM/vendor_boot_a.img | tail -1
  fastboot reboot recovery | tail -1
  wait_ofox
}

summary() {
  local t=$L/SHENGLOG-$1.txt
  grep -n 'elapsed=' "$t" | sed -n '1p;$p'
  echo "-- reinicios de GPU"; grep -n 'NativeAdreno recovery' "$t" | cut -c1-160 || true
  echo "-- trampa 0x215: $(grep -c 'Q br 215' "$t" || true) filas"
  echo "-- copia a pantalla"; grep 'UKE GOP: t' "$t" | awk 'NR%4==1' | cut -c1-140 || true
  echo "-- volcados de cuelgue"; grep -E 'UKE HANG[0-9]+:|UKE HANG walk|UKE HANG regs|UKE HANG smmu' "$t" | cut -c1-190 || true
}

case "${1:-}" in
pull)
  name=${2:?nombre del log, p. ej. M2y}
  to_ofox
  adb shell "dd if=/dev/block/sda bs=4096 skip=$LOG_LBA count=512 2>/dev/null" > $L/SHENGLOG-$name.bin
  cd $D/refs/macOS-ForSheng
  U=$(dwarfdump --uuid out/uke-prelink/kernel.development.sheng | awk '{print $2}' | head -1)
  cd scripts
  ../.venv/bin/python - "$U" "$L/SHENGLOG-$name" <<'E'
import sys, native_log_file as n
data = open(sys.argv[2] + '.bin', 'rb').read()
text = report = None
for x in n.decode(data, sys.argv[1]):
    if isinstance(x, dict): report = x
    else: text = x
open(sys.argv[2] + '.txt', 'w').write(report['console_text'] if report and 'console_text' in report else (text or b'').decode('utf-8', 'replace'))
print('retenido %s B, perdido %s B' % (report.get('retained_bytes'), report.get('omitted_bytes')))
E
  summary "$name"
  ;;
flash)
  img=${2:?imagen vendor_boot}
  [ -f "$img" ] || img=$D/boot/out/$img
  [ -f "$img" ] || { echo "no existe $2"; exit 1; }
  if in_ofox; then adb reboot bootloader; fi
  wait_fastboot
  fastboot flash boot_a $D/boot/out/boot.img | tail -1
  fastboot flash vendor_boot_a "$img" | tail -1
  fastboot set_active a | tail -1
  fastboot reboot | tail -1
  echo "arrancando $(basename "$img") $(date +%H:%M:%S)"
  ;;
guard)
  size=${2:?512k o 64k}
  in_ofox || { echo "la tablet tiene que estar en OrangeFox"; exit 1; }
  adb push -q $L/guard-$size.bin /tmp/guard.bin
  adb shell "dd if=/tmp/guard.bin of=/dev/block/sda bs=4096 seek=$GUARD_LBA count=1 conv=notrunc,fsync 2>/dev/null"
  adb shell "dd if=/dev/block/sda bs=4096 skip=$GUARD_LBA count=1 2>/dev/null | sha256sum" | cut -c1-16
  shasum -a 256 $L/guard-$size.bin | cut -c1-16
  ;;
summary) summary "${2:?nombre}" ;;
*) sed -n 2,7p "$0"; exit 1 ;;
esac
