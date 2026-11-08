#!/usr/bin/env bash
# Build the UEFI-only boot.img for the Xiaomi Pad 7 (uke). Run on the Mac.
#   ./boot/make-boot-uefi.sh [path/to/vendor_boot_a.img]   (default: boot/stock/vendor_boot_a.img)
set -euo pipefail
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
if command -v brew >/dev/null 2>&1; then
  export PATH="$(brew --prefix)/opt/python@3.12/bin:$(brew --prefix)/opt/llvm/bin:$(brew --prefix)/opt/lld/bin:$PATH"
fi
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
VB=${1:-$HERE/stock/vendor_boot_a.img}
UEFI=$ROOT/uefi-uke-fork
DEV=$ROOT/UniXNU-uke-fork
FD=${UKE_FD:-$UEFI/Build/ukePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd}
OUT=$HERE/out; mkdir -p "$OUT"

for t in python3 clang llvm-objcopy llvm-nm fdtput fdtget; do
  command -v $t >/dev/null || { echo "falta $t (brew install python@3.12 llvm lld dtc)"; exit 1; }
done
python3 -c 'import sys; sys.exit(sys.version_info<(3,11))' || { echo "se necesita python>=3.11"; exit 1; }
[ -f "$VB" ] || { echo "no existe $VB (haz fastboot fetch, ver boot/README.md)"; exit 1; }
[ -f "$FD" ] || { echo "no existe el FD $FD"; exit 1; }

python3 "$HERE/extract_vendor_dtb.py" "$VB" "$OUT/uke.dtb"
cd "$ROOT/UniXNU"
./unixnu bootimg -D "$DEV" --uefi "$UEFI" --fd "$FD" --dtb "$OUT/uke.dtb" \
    --ramdisk none -o "$OUT/boot.img"
shasum -a 256 "$OUT/boot.img" "$FD"
echo "OK -> $OUT/boot.img"
