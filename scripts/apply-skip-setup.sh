#!/bin/bash
# Attach the full-OS container image read-write, mount its Data volume (found by UUID, never by name), run
# macos-skip-setup.py on it, then unmount and detach. Usage: sudo boot/apply-skip-setup.sh
set -euo pipefail
D=${UKE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
IMG=$D/ipsw/macos26/fullos/25F71__MacOS/094-56453-087.dmg
DATA_UUID=$(cat $D/ipsw/macos26/fullos/data-uuid.txt)
MNT=/private/tmp/uke-fullos-data
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

whole=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nobrowse -nomount "$IMG" | awk 'NR==1{print $1}')
trap 'hdiutil detach "$whole" >/dev/null 2>&1 || true' EXIT
vol=$(diskutil apfs list -plist | python3 -I -c "
import plistlib, sys
for c in plistlib.loads(sys.stdin.buffer.read())['Containers']:
    for v in c['Volumes']:
        if v['APFSVolumeUUID'] == '$DATA_UUID':
            print(v['DeviceIdentifier'])")
[ -n "$vol" ] || { echo "Data volume $DATA_UUID not found"; exit 1; }
diskutil info "$vol" | grep -q "$DATA_UUID" || { echo "$vol is not $DATA_UUID"; exit 1; }
mkdir -p "$MNT"
diskutil mount -mountPoint "$MNT" "$vol"
python3 -I "$D/boot/macos-skip-setup.py" "$MNT"
ls -la "$MNT/Users"
diskutil unmount "$MNT"
