
      [ -e "$o/commits" ] || [ -e "$o/worktree.diff" ] || rm -rf "$o"#!/usr/bin/env bash
# Maintainer script: refresh patches/ from the author's working trees.
# Every repository the build uses is exported against a public commit, so setup-workspace.sh can rebuild it:
# local commits as `git format-patch` series, uncommitted edits as worktree.diff, new text files under new-files/.
# Firmware, keys and binaries are never exported (see check_clean below).
# Usage: UKE_ROOT=/path/to/workspace maint/export-patches.sh
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
D=${UKE_ROOT:-$(cd "$REPO/.." && pwd)}
P=$REPO/patches

# export NAME TREE OUTDIR [paths...]: base = last public commit, commits/ = local series, worktree.diff, new-files/
export_tree() {
  local tree=$D/$1 out=$P/$2; shift 2
  local base url
  base=$(git -C "$tree" merge-base HEAD "$(git -C "$tree" rev-list HEAD --not --remotes | tail -1)^" 2>/dev/null || git -C "$tree" rev-parse HEAD)
  url=$(git -C "$tree" remote get-url origin)
  rm -rf "$out"; mkdir -p "$out"
  echo "$url $(git -C "$tree" rev-parse "$base")" > "$out/BASE.txt"
  if [ "$(git -C "$tree" rev-parse "$base")" != "$(git -C "$tree" rev-parse HEAD)" ]; then
    git -C "$tree" format-patch -q --no-signature --zero-commit -o "$out/commits" "$base..HEAD" -- "$@"
  fi
  git -C "$tree" diff --ignore-submodules=all HEAD -- "$@" > "$out/worktree.diff"
  [ -s "$out/worktree.diff" ] || rm "$out/worktree.diff"
  git -C "$tree" ls-files --others --exclude-standard -- "$@" | { grep -vE '\.DS_Store$|\.bak|__pycache__|\.pyc$' || true; } |
    while read -r f; do
      file -b --mime "$tree/$f" | grep -q '^text/' || { echo "skip binary $1/$f"; continue; }
      mkdir -p "$out/new-files/$(dirname "$f")"; cp "$tree/$f" "$out/new-files/$f"
    done
  # submodules with their own local work (gitlink moved or dirty)
  git -C "$tree" submodule foreach --quiet 'echo $sm_path' 2>/dev/null | while read -r sm; do
    if [ -n "$(git -C "$tree/$sm" status --porcelain --untracked-files=no)" ] ||
       [ -n "$(git -C "$tree/$sm" rev-list HEAD --not --remotes 2>/dev/null)" ]; then
      local o=$out/submodules/$sm; mkdir -p "$o"
      local sb; sb=$(git -C "$tree/$sm" merge-base HEAD "$(git -C "$tree/$sm" rev-list HEAD --not --remotes | tail -1)^" 2>/dev/null || git -C "$tree/$sm" rev-parse HEAD)
      echo "$(git -C "$tree/$sm" remote get-url origin) $sb" > "$o/BASE.txt"
      [ "$sb" != "$(git -C "$tree/$sm" rev-parse HEAD)" ] && git -C "$tree/$sm" format-patch -q --no-signature --zero-commit -o "$o/commits" "$sb..HEAD"
      git -C "$tree/$sm" diff HEAD > "$o/worktree.diff"; [ -s "$o/worktree.diff" ] || rm "$o/worktree.diff"
    fi
  done
}

# UEFI boot.img (make-boot-uefi.sh): UniXNU core + iOSNJBrepo/UniXNU-uke device tree
export_tree UniXNU                  uefi-core
export_tree UniXNU-uke-fork         uefi-device
export_tree uefi-uke-fork           uefi
# UniXNU-sheng checkout that macOS-ForSheng's sources.py fetches (patches/forsheng and patches/unixnu cover the rest)
export_tree refs/macOS-ForSheng/UniXNU-sheng unixnu-sheng
# ADT and vendor_boot tools of the macOS image (make-macos-image.sh): only the two scripts it runs
export_tree UniXNU-uke-kbuild-ios26 device scripts/make-uke-adt.py scripts/patch-vendor-boot.py
# iPadOS 27 GPU baseline the macOS GPU sources come from (gpu/stage.py, build-plugin/turnip)
export_tree UniXNU-a27              a27-core
export_tree UniXNU-uke-allenwdk     a27-device
export_tree uefi-uke-a27            a27-uefi

# submodules that are only dirty through their own nested submodules or untracked files: nothing to export
find "$P" -path '*/submodules/*' -name BASE.txt | while read -r b; do
  [ "$(ls -A "$(dirname "$b")")" = BASE.txt ] && rm -rf "$(dirname "$b")"
done
find "$P" -type d -empty -delete

check_clean() {
  local bad
  bad=$(grep -rlIE 'BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY|eluniversoapple|@gmail\.com' "$P" || true)
  [ -z "$bad" ] || { echo "private material in: $bad"; exit 1; }
  bad=$(grep -rlE '^GIT binary patch' "$P" || true)
  [ -z "$bad" ] || { echo "binary patch in: $bad"; exit 1; }
}
check_clean
echo "patches exported to $P"
