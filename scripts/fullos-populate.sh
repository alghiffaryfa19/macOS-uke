#!/bin/bash
# Fill the rebuilt full-OS container in one go (run once, with sudo, after fullos-volumes.sh made the volumes):
#   1. Data      <- System/Library/Templates/Data (what the installer seeds)
#   2. System    <- the SystemOS cryptex dyld cache at /System/Library/dyld (no ignition: dyld looks only there)
#   3. Preboot   <- the rest of the cryptex at /Cryptexes/OS (System/Cryptexes/OS links there)
#   4. System    <- the cryptex's standalone files at their real paths, never overwriting (libLaunchServicesSupport...)
#   5. Data      <- macos-skip-setup.py (user uke, auto-login, Spanish, no Setup Assistant)
#   6. Data      <- Metal phase 2: /Library/GPUBundles/NativeAdrenoMetal.bundle -> /Volumes/UKEDRV/... and an fstab
#                   line that mounts the DRV volume there (both inert until the DRV window and SIP0=0x7f are used)
# Volumes are found by UUID/role inside the container of THIS image only, never by name.
# Usage: sudo boot/fullos-populate.sh
set -euo pipefail
D=${UKE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
F=$D/ipsw/macos26/fullos
IMG=$F/25F71__MacOS/094-56453-087.dmg
CX=$F/25F71__MacOS/094-56679-089.dmg
SYS_UUID=47719F50-5527-44A3-9F59-8170528D8C14
DATA_UUID=$(cat "$F/data-uuid.txt")
T=/private/tmp/uke-fullos
[ "$(id -u)" = 0 ] || { echo "ejecútalo con sudo"; exit 1; }
[ -f "$IMG" ] && [ -f "$CX" ] || { echo "falta la imagen del sistema o el cryptex"; exit 1; }

whole=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount -nobrowse "$IMG" | awk 'NR==1{print $1}')
mkdir -p $T/cx $T/sys $T/data $T/preboot
cleanup() {
  for m in $T/data $T/preboot $T/sys; do diskutil unmount "$m" >/dev/null 2>&1 || true; done
  hdiutil detach $T/cx -quiet 2>/dev/null || true
  hdiutil detach "$whole" -quiet 2>/dev/null || true
}
trap cleanup EXIT
hdiutil attach -readonly -nobrowse -noverify -mountpoint $T/cx "$CX" >/dev/null

# volume <UUID|role:Preboot>: the device of that volume in the container whose physical store is this image
volume() {
  diskutil apfs list -plist | python3 -I -c "
import plistlib, sys
want, store = sys.argv[1], sys.argv[2]
for c in plistlib.loads(sys.stdin.buffer.read())['Containers']:
    if not any(p['DeviceIdentifier'].startswith(store) for p in c.get('PhysicalStores', [])):
        continue
    for v in c['Volumes']:
        if v['APFSVolumeUUID'] == want or (want.startswith('role:') and want[5:] in v.get('Roles', [])):
            print(v['DeviceIdentifier'])" "$1" "$(basename "$whole")"
}
sys=$(volume $SYS_UUID); data=$(volume "$DATA_UUID"); preboot=$(volume role:Preboot)
echo "imagen $whole: System=$sys Data=$data Preboot=$preboot"
[ -n "$sys" ] && [ -n "$data" ] && [ -n "$preboot" ] || { echo "no encuentro los tres volúmenes en $whole"; exit 1; }
diskutil info "$sys" | grep -q $SYS_UUID && diskutil info "$data" | grep -q "$DATA_UUID" || { echo "UUID no coincide"; exit 1; }

diskutil mount -mountPoint $T/sys "$sys"
diskutil mount -mountPoint $T/data "$data"
diskutil mount -mountPoint $T/preboot "$preboot"
for m in $T/sys $T/data $T/preboot; do diskutil enableOwnership $m; done

echo "1/6 Data desde Templates/Data"
ditto $T/sys/System/Library/Templates/Data $T/data
echo "2/6 caché dyld (6,1 GB)"
mkdir -p $T/sys/System/Library/dyld
ditto $T/cx/System/Library/dyld $T/sys/System/Library/dyld
echo "3/6 resto del cryptex en Preboot"
mkdir -p $T/preboot/Cryptexes/OS
rsync -a --exclude /System/Library/dyld $T/cx/ $T/preboot/Cryptexes/OS/
echo "4/6 ficheros del cryptex en el sistema"
rsync -a --ignore-existing --exclude /System/Library/dyld $T/cx/ $T/sys/
echo "5/6 sin asistente de configuración"
python3 -I $D/boot/macos-skip-setup.py $T/data
echo "6/6 enlace GPUBundles y fstab de UKEDRV"
mkdir -p $T/data/Library/GPUBundles
ln -sfn /Volumes/UKEDRV/NativeAdrenoMetal.bundle $T/data/Library/GPUBundles/NativeAdrenoMetal.bundle
mkdir -p $T/data/private/etc
grep -qs UKEDRV $T/data/private/etc/fstab || echo "LABEL=UKEDRV /Volumes/UKEDRV apfs ro,nobrowse" >> $T/data/private/etc/fstab
chown root:wheel $T/data/private/etc/fstab; chmod 644 $T/data/private/etc/fstab
ls -la $T/data/Users $T/data/Library/GPUBundles
du -sh $T/sys/System/Library/dyld $T/preboot/Cryptexes/OS
echo HECHO
