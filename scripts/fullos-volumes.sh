#!/bin/bash
# Turn the IPSW system container (094-56453-087.dmg, raw APFS, one unsealed System volume) into a full-OS container,
# as on 2026-11-02 but in one step: grow it to the size the DRV layout assumes (22735224832 B, so the OS window stays
# 22737321984 B and the ISC/DRV offsets do not move), add Data (D), Preboot (B) and VM (V), write the Data UUID to
# data-uuid.txt and group Data with System by hand (diskutil -groupWith refuses on the IPSW volume, -69624).
# No sudo needed. Next: sudo boot/fullos-populate.sh, then boot/macos-full-root.py plan.
set -euo pipefail
D=${UKE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
F=$D/ipsw/macos26/fullos
IMG=$F/25F71__MacOS/094-56453-087.dmg
SYS_UUID=47719F50-5527-44A3-9F59-8170528D8C14
SIZE=22735224832
[ "$(stat -f %z "$IMG")" = 13071548416 ] || { echo "$IMG no es la imagen original de 13071548416 B"; exit 1; }
truncate -s $SIZE "$IMG"
whole=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount -nobrowse "$IMG" | awk 'NR==1{print $1}')
trap 'hdiutil detach "$whole" -quiet 2>/dev/null || true' EXIT
container_of() {
  diskutil apfs list -plist | python3 -I -c "
import plistlib, sys
for c in plistlib.loads(sys.stdin.buffer.read())['Containers']:
    if any(p['DeviceIdentifier'] == sys.argv[1] for p in c.get('PhysicalStores', [])):
        print(c['ContainerReference'])" "$(basename "$1")"
}
container=$(container_of "$whole")
[ -n "$container" ] || { echo "no hay contenedor en $whole"; exit 1; }
diskutil apfs list "$container" | grep -q $SYS_UUID || { echo "$container no tiene el System $SYS_UUID"; exit 1; }
echo "imagen $whole, contenedor $container"
diskutil apfs resizeContainer "$container" 0
diskutil apfs addVolume "$container" APFS Data -role D -nomount
diskutil apfs addVolume "$container" APFS Preboot -role B -nomount
diskutil apfs addVolume "$container" APFS VM -role V -nomount
data=$(diskutil apfs list -plist | python3 -I -c "
import plistlib, sys
for c in plistlib.loads(sys.stdin.buffer.read())['Containers']:
    if c['ContainerReference'] == sys.argv[1]:
        for v in c['Volumes']:
            if 'Data' in v.get('Roles', []): print(v['APFSVolumeUUID'])" "$container")
[ -n "$data" ] || { echo "no se creó Data"; exit 1; }
echo "$data" > "$F/data-uuid.txt"
hdiutil detach "$whole" -quiet
trap - EXIT
python3 -I $D/boot/apfs-set-volume-group.py "$IMG" $SYS_UUID "$data"
whole=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount -nobrowse "$IMG" | awk 'NR==1{print $1}')
diskutil apfs listVolumeGroups "$(container_of "$whole")" | head -20
hdiutil detach "$whole" -quiet
echo "Data $data agrupado con System"
