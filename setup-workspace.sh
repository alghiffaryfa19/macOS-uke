#!/usr/bin/env bash
# Rebuild the workspace this port is developed in, from public repositories plus the patches in this repo.
#
#   ./setup-workspace.sh [UKE_ROOT] [step...]
#
# UKE_ROOT defaults to the parent directory of this repository. Steps (default: all, in this order):
#   boot      copy scripts/, gpu/, touch/ and docs/ into UKE_ROOT/boot (the layout every script expects)
#   forsheng  macOS-ForSheng (refs/macOS-ForSheng) with its UniXNU and UniXNU-sheng checkouts
#   device    Allenwdk/UniXNU-uke tools used by make-macos-image.sh (UniXNU-uke-kbuild-ios26)
#   uefi      the UniXNU UEFI boot.img sources (uefi-uke-fork, UniXNU-uke-fork, UniXNU)
#   a27       the iPadOS 27 GPU baseline the macOS GPU driver is staged from (UniXNU-a27, UniXNU-uke-allenwdk, uefi-uke-a27)
#
# Nothing here downloads Apple software or firmware: see INSTALL.md for what you must provide yourself.
# A step never touches a checkout that already exists; delete it first to redo it.
set -euo pipefail
REPO=$(cd "$(dirname "$0")" && pwd)
D=${1:-${UKE_ROOT:-$(cd "$REPO/.." && pwd)}}; [ $# -gt 0 ] && shift
STEPS=${*:-boot forsheng device uefi a27}
P=$REPO/patches
mkdir -p "$D"; D=$(cd "$D" && pwd)
# git am needs a committer; the original authors are kept.
export GIT_COMMITTER_NAME=${GIT_COMMITTER_NAME:-uke-setup} GIT_COMMITTER_EMAIL=${GIT_COMMITTER_EMAIL:-uke-setup@localhost}
say() { printf '\n== %s\n' "$*"; }

# clone DIR PATCHDIR: clone the repository named in PATCHDIR/BASE.txt at its commit, with submodules
clone() {
  local dir=$D/$1 url commit
  read -r url commit < "$2/BASE.txt"
  if [ -e "$dir" ]; then echo "exists, left alone: $dir"; return 1; fi
  git clone --quiet "$url" "$dir"
  git -C "$dir" checkout --quiet -B uke-local "$commit"
  git -C "$dir" submodule update --quiet --init --recursive
}

# apply TREE PATCHDIR: local commits, uncommitted edits, new files, then the same for each patched submodule
apply() {
  local tree=$1 pd=$2 sm
  if [ -d "$pd/submodules" ]; then
    (cd "$pd/submodules" && find . -name BASE.txt | sed 's|^\./||; s|/BASE.txt$||') | while read -r sm; do
      local want; read -r _ want < "$pd/submodules/$sm/BASE.txt"
      git -C "$tree/$sm" checkout --quiet "$want"
      apply "$tree/$sm" "$pd/submodules/$sm"
    done
  fi
  if [ -d "$pd/commits" ]; then
    git -C "$tree" am --quiet --3way "$pd"/commits/*.patch
  fi
  # git am recreated the submodule commits with new ids: pin the core to them
  if [ -d "$pd/submodules" ]; then
    (cd "$pd/submodules" && find . -name BASE.txt | sed 's|^\./||; s|/BASE.txt$||') | while read -r sm; do
      if [ -d "$pd/submodules/$sm/commits" ]; then git -C "$tree" add "$sm"; fi
    done
    git -C "$tree" diff --cached --quiet || git -C "$tree" commit --quiet -m "setup: pin the rebuilt submodule commits"
  fi
  if [ -f "$pd/worktree.diff" ]; then git -C "$tree" apply --whitespace=nowarn "$pd/worktree.diff"; fi
  if [ -d "$pd/new-files" ]; then cp -R "$pd/new-files/." "$tree/"; fi
}

# get DIR PATCHDIR: clone + apply
get() { if clone "$1" "$2"; then apply "$D/$1" "$2"; echo "ok $1"; fi; }

for step in $STEPS; do case $step in
boot)
  say "boot: scripts into $D/boot"
  mkdir -p "$D/boot/macos-gpu" "$D/boot/macos-touch" "$D/boot/out"
  rsync -a "$REPO/scripts/" "$D/boot/"
  rsync -a "$REPO/gpu/" "$D/boot/macos-gpu/"
  rsync -a "$REPO/touch/" "$D/boot/macos-touch/"
  rsync -a "$REPO/docs/" "$D/boot/"
  ;;
forsheng)
  say "forsheng: macOS-ForSheng + UniXNU + UniXNU-sheng"
  FS=$D/refs/macOS-ForSheng; mkdir -p "$D/refs"
  if [ -e "$FS" ]; then echo "exists, left alone: $FS"; else
    git clone --quiet https://github.com/CrossDarwin/macOS-ForSheng.git "$FS"
    git -C "$FS" checkout --quiet -B uke-macos ca57db7
    git -C "$FS" am --quiet --3way "$P"/forsheng/commits/*.patch
    git -C "$FS" apply --whitespace=nowarn "$P/forsheng/worktree.diff"
    cp -R "$P/forsheng/new-files/." "$FS/"
    # UniXNU and UniXNU-sheng are plain checkouts inside ForSheng (gitignored), at the commits its sources.lock.json pins.
    # patches/unixnu already holds the state ForSheng's scripts/apply_patches.py would produce: do not run it.
    git clone --quiet https://github.com/UniXNU/UniXNU.git "$FS/UniXNU"
    git -C "$FS/UniXNU" checkout --quiet -B uke-local "$(awk '$1=="UniXNU"{print $2}' "$P/unixnu/BASE.txt")"
    git -C "$FS/UniXNU" submodule update --quiet --init --recursive
    awk 'NF==3' "$P/unixnu/BASE.txt" | while read -r sm commit _; do
      git -C "$FS/UniXNU/$sm" checkout --quiet "$commit"
      d=$P/unixnu/$(basename "$sm").diff
      if [ -s "$d" ]; then git -C "$FS/UniXNU/$sm" apply --whitespace=nowarn "$d"; fi
    done
    git -C "$FS/UniXNU" apply --whitespace=nowarn "$P/unixnu/UniXNU.diff"
    # new-files/UniXNU/ holds files of the core itself (the core has an `unixnu` tool, and macOS paths ignore case)
    cp -R "$P/unixnu/new-files/UniXNU/." "$FS/UniXNU/"
    cp -R "$P/unixnu/new-files/drivers" "$FS/UniXNU/"
    clone refs/macOS-ForSheng/UniXNU-sheng "$P/unixnu-sheng" && apply "$FS/UniXNU-sheng" "$P/unixnu-sheng"
    echo "ok refs/macOS-ForSheng (next: its .venv and Apple open-source trees, INSTALL.md step 3)"
  fi
  ;;
device)
  say "device: ADT and vendor_boot tools"
  get UniXNU-uke-kbuild-ios26 "$P/device"
  ;;
uefi)
  say "uefi: UniXNU UEFI for boot_a"
  get uefi-uke-fork   "$P/uefi"
  get UniXNU-uke-fork "$P/uefi-device"
  get UniXNU          "$P/uefi-core"
  ;;
a27)
  say "a27: iPadOS 27 GPU baseline"
  get UniXNU-a27          "$P/a27-core"
  get UniXNU-uke-allenwdk "$P/a27-device"
  get uefi-uke-a27        "$P/a27-uefi"
  [ -e "$D/UniXNU-a27/unixnu.local.toml" ] || cat > "$D/UniXNU-a27/unixnu.local.toml" <<'EOF'
[build]
device = "../UniXNU-uke-allenwdk"
uefi = "../uefi-uke-a27"
dtb = "../boot/out/uke.dtb"
jobs = 10
EOF
  ;;
*) echo "unknown step $step"; exit 2 ;;
esac; done
say "done: $D"
