#!/bin/bash
# Put the Metal plugin straight into the full-OS Data volume: /Library/GPUBundles/NativeAdrenoMetal.bundle becomes a real
# directory (root:wheel, not writable by others) instead of the symlink to /Volumes/UKEDRV. diskarbitrationd never mounted
# the DRV volume on the tablet (logs M2h..M2j 2026-11-07, no console trace of why), and a kernel mount of APFS is refused
# (every mount from kernel_task is a root mount), so the plugin no longer depends on any mount. The binaries are the ones
# already in boot/out/full-os-drv.trustcache (cdhashes 9421db53..., 037e3179...). The DRV window still holds the GPU
# firmware. Volumes are found by UUID inside this image's container only. Usage: sudo boot/install-gpubundle.sh
set -euo pipefail
D=${UKE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
IMG=$D/ipsw/macos26/fullos/25F71__MacOS/094-56453-087.dmg
DATA_UUID=$(cat $D/ipsw/macos26/fullos/data-uuid.txt)
SRC=$D/boot/out/drv-stage/NativeAdrenoMetal.bundle
M=/private/tmp/uke-fullos-data
[ "$(id -u)" = 0 ] || { echo "ejecútalo con sudo"; exit 1; }
[ -d "$SRC/Contents/MacOS" ] || { echo "falta $SRC"; exit 1; }
whole=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nobrowse -nomount "$IMG" | awk 'NR==1{print $1}')
trap 'diskutil unmount "$M" >/dev/null 2>&1 || true; hdiutil detach "$whole" -quiet 2>/dev/null || true' EXIT
data=$(diskutil apfs list -plist | python3 -I -c "
import plistlib, sys
for c in plistlib.loads(sys.stdin.buffer.read())['Containers']:
    if any(p['DeviceIdentifier'].startswith(sys.argv[2]) for p in c.get('PhysicalStores', [])):
        for v in c['Volumes']:
            if v['APFSVolumeUUID'] == sys.argv[1]: print(v['DeviceIdentifier'])" "$DATA_UUID" "$(basename "$whole")")
[ -n "$data" ] || { echo "no encuentro Data $DATA_UUID"; exit 1; }
mkdir -p "$M"
diskutil mount -mountPoint "$M" "$data"
diskutil enableOwnership "$M"
G="$M/Library/GPUBundles"
mkdir -p "$G"
rm -rf "$G/NativeAdrenoMetal.bundle"
ditto --norsrc --noextattr "$SRC" "$G/NativeAdrenoMetal.bundle"
# Phase 2b marker: with NativeAdrenoMetal.publish beside the bundle, WindowServer gets the native device (build-plugin-macos.py --publish).
if [ -f "$(dirname "$SRC")/NativeAdrenoMetal.publish" ]; then : > "$G/NativeAdrenoMetal.publish"; else rm -f "$G/NativeAdrenoMetal.publish"; fi
# Pipeline pack primed on the Mac (boot/macos-gpu/hostcache: run-prime.sh, make-pack.py): WindowServer imports it before its device opens.
if [ -f "$(dirname "$SRC")/NativeAdrenoMetal.m2vpack" ]; then ditto --norsrc --noextattr "$(dirname "$SRC")/NativeAdrenoMetal.m2vpack" "$G/NativeAdrenoMetal.m2vpack"; else rm -f "$G/NativeAdrenoMetal.m2vpack"; fi
# The libraries WindowServer primes (the pack's): without the list it walks every metallib of /System/Library at each boot.
if [ -f "$(dirname "$SRC")/NativeAdrenoMetal.primelist" ]; then ditto --norsrc --noextattr "$(dirname "$SRC")/NativeAdrenoMetal.primelist" "$G/NativeAdrenoMetal.primelist"; else rm -f "$G/NativeAdrenoMetal.primelist"; fi
chown -R root:wheel "$G"
chmod -R go-w "$G"
ls -la "$G" "$G/NativeAdrenoMetal.bundle/Contents/MacOS"
for f in "$G"/NativeAdrenoMetal.bundle/Contents/MacOS/*; do codesign -dvvv "$f" 2>&1 | grep CDHash=; done
sync
echo HECHO
