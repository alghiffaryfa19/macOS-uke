#!/usr/bin/env bash
# vendor_boot for the macOS 26.5 kernel (macOS-ForSheng build, prelinked). The uke ABL ignores a boot.img
# ramdisk (RAM boot gave "failed to find operating system"), so the 110 MB kernel loses its kexts'
# __PLK_LINKEDIT (strip-plk-linkedit.py, 82 MB) to fit the 96 MiB vendor_boot.
#   out/macos_payload.fat     FAT: EFI/UniXNU/{unixnu.toml,Payloads/kernel.macho,kernel.macho.adt}
#   out/vendor_boot_uke_macos.img  the stock vendor_boot with that FAT as vendor ramdisk
# Usage: [GOP=1] [FAT_MIB=92] boot/make-macos-image.sh [extra boot args...]
#   GOP=1: the ADT enables the NativeGOPDisplay framebuffer (pass native-gop-display=1 too)
set -euo pipefail
D=/Users/daniel/Desktop/Deliver/Domotica
FS=$D/refs/macOS-ForSheng
DEV=$D/UniXNU-uke-kbuild-ios26
OUT=$D/boot/out
KERNEL=$OUT/macos-kernel-stripped.macho
python3 -I $D/boot/strip-plk-linkedit.py $FS/out/uke-prelink/kernel.development.sheng $KERNEL
ROOT_UUID=808EAC5E-8275-478D-814C-2229D8EB8D07
OS_ENV=recoveryos
# CPUS=2/4/8: secondary cores as macOS-ForSheng starts them (MPIDR 0x000..0x700, PSCI SMC, SPI IPIs 1008+).
CPU_REGS=0
case "${CPUS:-1}" in
  2) CPU_REGS=0,0x100 ;; 4) CPU_REGS=0,0x100,0x200,0x300 ;; 8) CPU_REGS=0,0x100,0x200,0x300,0x400,0x500,0x600,0x700 ;;
esac
SMP_ARGS=""; [ "${CPUS:-1}" != 1 ] && SMP_ARGS="sheng-smp=1 armcpu-spi-ipi=1"   # not armcpu-self-ipi=1: with the SPI route the secondaries never enter the kernel (logs F8, F9)
# FULL=1: full macOS (unsealed IPSW system volume + Data, boot/macos-full-root.py): main environment, no root hash.
[ "${FULL:-0}" = 1 ] && ROOT_UUID=47719F50-5527-44A3-9F59-8170528D8C14 && OS_ENV=main
ARGS="-v debug=0x14e keepsyms=1 uke-epoch=$(date +%s) cpus=${CPUS:-1} -enable_kprintf_spam -sheng-early-console native-ufs-root=1 native-ufs-diagnostics=1 -sheng-development-no-ignition $SMP_ARGS ${*:-}"
STAGE=$(mktemp -d "$OUT/macos-stage.XXXX")
trap 'rm -rf "$STAGE"' EXIT
P=$STAGE/EFI/UniXNU/Payloads
mkdir -p "$P"

# TRUSTCACHE=file: static trust cache handed over by DarwinBoot (kernel.macho.trustcache + ADT memory-map TrustCache).
[ -n "${TRUSTCACHE:-}" ] && cp "$TRUSTCACHE" "$P/kernel.macho.trustcache"
# DarwinBoot takes only cpusubtype 0; the kernel applies its own (format 7) fixups.
cp "$KERNEL" "$P/kernel.macho"
printf '\0\0\0\0' | dd of="$P/kernel.macho" bs=1 seek=8 conv=notrunc 2>/dev/null

python3 -I "$DEV/scripts/make-uke-adt.py" --output "$P/kernel.macho.adt" --dram-size 0x240000000 \
  --dram-base 0x8C0000000 --owned-memory-size 0x130000000 --chip-id 207 \
  --cpu-regs $CPU_REGS --os-environment $OS_ENV --root-match-volume-uuid $ROOT_UUID \
  --unique-chip-id "$(tr -d ' \n' < "$D/boot/chip-id.txt")" ${TRUSTCACHE:+--trust-cache}
"$FS/.venv/bin/python" - "$P/kernel.macho.adt" "$D/ipsw/macos26/basesystem.root_hash.bin" $ROOT_UUID "${GOP:-0}" "${FULL:-0}" <<'EOF'
import os, sys, uuid
from pathlib import Path
sys.path.insert(0, '/Users/daniel/Desktop/Deliver/Domotica/refs/macOS-ForSheng/scripts')
from native_devtree import configure_native_text_state
sip0 = int(os.environ.get('SIP0', '0'), 0)   # lp-sip0 csr flags; 0x7f is "csrutil disable" (launchd then honours disabled.plist)
if sys.argv[5] == '1':   # full macOS: unsealed system volume, so no recovery root hash or boot volume identity
    configure_native_text_state(Path(sys.argv[1]), development_root=True, native_gop_display=sys.argv[4] == '1', sip0=sip0)
else:
    configure_native_text_state(Path(sys.argv[1]), development_root=True, root_hash=Path(sys.argv[2]).read_bytes(),
                                boot_volume_uuid=uuid.UUID(sys.argv[3]), native_gop_display=sys.argv[4] == '1', sip0=sip0)
print(f'adt: native text state configured, lp-sip0={sip0:#x}')
EOF

cat > "$STAGE/EFI/UniXNU/unixnu.toml" <<EOF
version = 1
[boot]
timeout = 3
default = "macos"
[kits]
autoload = false
[[entry]]
id = "macos"
title = "uke macOS 26.5 BaseSystem (RAM)"
type = "xnu"
path = "Payloads/kernel.macho"
args = "$ARGS"
EOF

FAT=$OUT/macos_payload.fat
rm -f "$FAT"
export MTOOLS_SKIP_CHECK=1 TZ=UTC
mformat -C -c ${FAT_CLUSTER:-16} -T $((${FAT_MIB:-94} * 2048 + ${FAT_EXTRA_SECTORS:-0})) -v UKEMACOS -N 53484e47 -i "$FAT" ::
mcopy -i "$FAT" -s -Q -m "$STAGE/EFI" ::/
# mcopy can report "Disk full" and still exit 0: check the kernel really landed in the FAT.
got=$(MTOOLS_SKIP_CHECK=1 mdir -i "$FAT" ::/EFI/UniXNU/Payloads/kernel.macho 2>/dev/null | awk '/kernel/ {print $3}' | tr -d ' ')
[ "$got" = "$(stat -f %z "$P/kernel.macho")" ] || { echo "kernel did not fit in the FAT ($got)"; exit 1; }

python3 -I "$DEV/scripts/patch-vendor-boot.py" \
  --base "$D/boot/rom-extract/uke_global_images_OS2.0.102.0.VOZMIXM_15.0/images/vendor_boot.img" \
  --payload "$FAT" --output "$OUT/vendor_boot_uke_macos.img"
