# Installing macOS 26.5 on the Xiaomi Pad 7 (uke)

This guide rebuilds, from public sources plus this repository, the setup the author runs on their own tablet. It is a research port, not a product: read the whole guide before you start, and expect to debug.

> **Warning.** This wipes `userdata` (and anything else on the tablet you choose to overwrite). It needs an unlocked bootloader. **Never re-lock the bootloader** while a non-stock `boot`/`vendor_boot` is flashed, and **never flash firmware from an older ROM** (anti-rollback). Back up your stock partitions first: the author had no stock backup on the tablet and had to rely on dumps made earlier.

## 0. What you need

**Hardware**
- A Xiaomi Pad 7 (`uke`, SM7675) with an unlocked bootloader. The author's unit has 128 GB of storage. The block offsets below (the log FAT, the DRV window) are for that layout: check yours with `sgdisk -p` and adjust them.
- The touch firmware is selected per panel. The author's panel is Tianma (`o82_36_02_0a`, so `novatek_nt36532_o82_fw_tm.bin`); CSOT panels need `..._fw_csot.bin`.
- A USB-C cable. The fastboot/adb host is the Mac.

**Host**
- A Mac on Apple silicon running macOS 26 or later, with Xcode and the macOS 26 SDK, about 120 GB free, and Homebrew.
  - From Homebrew: `python@3.12`, `llvm`, `lld`, `dtc`, `mtools`, `cmake`, `ninja`, `meson`, `rsync`, `uv`, `android-platform-tools`.
  - [`ipsw`](https://github.com/blacktop/ipsw).
  - A Rust nightly with `rust-src` (for the Metal bridge's `-Z build-std`).

**Inputs you must obtain yourself.** None of them is in this repository, and they must not be published:

| Input | Where it goes | How |
| --- | --- | --- |
| macOS 26.5 (25F71) system DMG `094-56453-087.dmg` and SystemOS cryptex `094-56679-089.dmg` | `ipsw/macos26/fullos/25F71__MacOS/` | `ipsw extract --remote` from Apple's 25F71 IPSW, then `ipsw fw aea` to decrypt (`--pem` with the published key for the cryptex) |
| KDK for 25F71 | `ipsw/macos26/kdk/expanded/` | Apple Developer downloads; expand the pkg so `KDK.pkg/Payload` and `KDK_SDK.pkg/Payload` exist |
| iPadOS 27.0.1 (24A446) inputs | `UniXNU-a27/out/ipsw/24A446` | `./unixnu fetch` in step 5 |
| Stock ROM: `uke_global_images_OS2.0.102.0.VOZMIXM_15.0` (fastboot ROM) | `boot/rom-extract/uke_global_images_OS2.0.102.0.VOZMIXM_15.0/` | Xiaomi's fastboot ROM for your region; `images/vendor_boot.img` is the base the macOS image is packed into |
| Your tablet's stock `boot.img` / `vendor_boot.img` | anywhere, set `UKE_ROM=<dir>` | `fastboot fetch` / OrangeFox `dd` |
| GPU firmware `gen71100_gmu.bin`, `gen71100_sqe.fw`, `gen71100_zap.mbn` | `UniXNU-uke-allenwdk/.cache/firmware/uke-gpu/` | from the tablet's `/vendor/firmware` and `/odm/firmware`; hashes in `UniXNU-uke-allenwdk/proprietary-files.txt` (`scripts/extract-files.py`) |
| Touch firmware `novatek_nt36532_o82_fw_{tm,csot}.bin` | `UniXNU-uke-allenwdk/.cache/firmware/novatek/` | same |
| Unique chip id | `boot/chip-id.txt` | your unit's ECID |
| OrangeFox recovery for uke | `boot/ofox/` | community build; it is only ever booted, never flashed |

## 1. Workspace

Every script finds the workspace root from its own location (`boot/..`), or from `UKE_ROOT` if it is set. The layout is:

```
UKE_ROOT/
  boot/                      scripts/ + docs/ of this repo, boot/macos-gpu = gpu/, boot/macos-touch = touch/, boot/out = outputs
  refs/macOS-ForSheng/       CrossDarwin/macOS-ForSheng + patches/forsheng, with UniXNU/ and UniXNU-sheng/ inside
  UniXNU-uke-kbuild-ios26/   Allenwdk/UniXNU-uke (make-uke-adt.py, patch-vendor-boot.py) + patches/device
  uefi-uke-fork/  UniXNU-uke-fork/  UniXNU/        the UniXNU UEFI for boot_a + patches/uefi*
  UniXNU-a27/  UniXNU-uke-allenwdk/  uefi-uke-a27/ the iPadOS 27 GPU baseline + patches/a27-*
  ipsw/macos26/              Apple inputs (step 0)
```

Create it:

```bash
git clone https://github.com/iOSNJBrepo/macOS-uke.git ~/uke/macOS-uke
```

```bash
~/uke/macOS-uke/setup-workspace.sh ~/uke
```

The script clones each repository at the public commit named in `patches/*/BASE.txt`. It then replays the local commits (`git am`), the uncommitted edits (`worktree.diff`) and the new files, re-pins rebuilt submodules, and copies this repo's scripts into `boot/`. Existing checkouts are left alone, and you can run single steps (`boot forsheng device uefi a27`). The result was checked file by file against the author's trees.

One file is not exported because it is binary: copy `uefi-uke-fork/Binaries/uke/RawFiles/SecParti.cfg` into `uefi-uke-a27/Binaries/uke/RawFiles/`.

## 2. UEFI (`boot_a`)

1. Build the UEFI firmware volume:
   ```bash
   cd ~/uke/uefi-uke-fork && ./setup_env.sh && python3 build_uefi.py -d uke
   ```
   See its `Building.md` for details. The output is `Build/ukePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd`.
2. Pack it into `boot.img` with the DTB of your stock `vendor_boot`:
   ```bash
   ~/uke/boot/make-boot-uefi.sh /path/to/stock/vendor_boot_a.img
   ```
   The output is `boot/out/boot.img` and `boot/out/uke.dtb`.

## 3. macOS-ForSheng environment and the kernel sources

```bash
cd ~/uke/refs/macOS-ForSheng
uv venv --python 3.13 .venv && uv pip install --python .venv/bin/python -r requirements.txt
python3 scripts/sources.py fetch --lock config/macos26-sources.json --group macos26
```

`setup-workspace.sh` already placed `UniXNU/` and `UniXNU-sheng/` at the pinned commits with our changes. Do **not** run `scripts/sources.py fetch --group all` over them, and do **not** run `scripts/apply_patches.py`: `patches/unixnu` already contains its result. See ForSheng's `docs/bringup.md` for the rest of its toolchain.

## 4. The full macOS system image

Follow `boot/MACOS-RETURN.md` (written for the author's tablet, in Spanish). The short version:

1. Generate the empty ISC with `scripts/prepare_qemu_isc.py --output <new dir>`, and move `isc.raw` to `ipsw/macos26/fullos/isc-window.img`.
2. Run `boot/fullos-volumes.sh` (no sudo). It grows the system container to 22735224832 B and adds the Data, Preboot and VM volumes.
3. Run `sudo boot/fullos-populate.sh`. It fills Data from the templates, installs the dyld cache and the cryptex contents, skips Setup Assistant (local user `uke`, password `uke`) and adds the `/Library/GPUBundles` link and the UKEDRV fstab line.
4. Run `python3 -I boot/macos-full-root.py plan`, which prints the OS and ISC window digests. Put them in ForSheng's `config/macos26-recovery-root.json`.

## 5. GPU: iPadOS 27 baseline, then the macOS driver

The macOS Adreno 732 driver is staged from a local iPadOS 27 GPU build, so build that tree first:

```bash
cd ~/uke/UniXNU-a27
./unixnu defconfig uke_ipados27_defconfig -D ../UniXNU-uke-allenwdk
./unixnu fetch -D ../UniXNU-uke-allenwdk
./unixnu build kernel -D ../UniXNU-uke-allenwdk
./unixnu build payload -D ../UniXNU-uke-allenwdk
```

This yields `out/uke/ipados-27.0.1-24A446/uke_ipados27/tree`, with the patched Mesa and the Metal bridge sources. `unixnu.local.toml` (written by the setup script) points it at the device tree, the UEFI and `boot/out/uke.dtb`. `unixnu build` resets submodules to the core's gitlinks; the setup script pins them to the rebuilt commits.

The commits in `patches/a27-device` are the author's unit settings: 128 GB userdata, their own GPU zap pin, their owner boot-set identity and the Data seed UUIDs. Review them against your unit.

Then, from `~/uke`:

```bash
refs/macOS-ForSheng/.venv/bin/python -I boot/macos-gpu/stage.py
```
This copies the kernel GPU sources to `boot/macos-gpu/src` and applies `gpu/patches/`. The generated `src/firmware.S` holds your firmware: never commit it.

```bash
python3 -I boot/macos-gpu/build-turnip-macos.py
```
This builds Turnip (arm64e, macOS 26.5).

```bash
python3 -I boot/macos-gpu/build-plugin-macos.py --publish
```
This builds `NativeAdrenoMetal.bundle` (plugin, translator and Turnip) and stages it in `boot/out/drv-stage`.

```bash
python3 -I boot/macos-gpu/make-drv.py image boot/out/drv-stage boot/out/drv.img
```
This makes the 512 MiB DRV window (APFS `UKEDRV` plus the firmware table).

```bash
python3 -I boot/macos-gpu/make-drv.py manifest boot/out/manifest-drv.bin
```
This makes the userdata header with the OS, ISC and DRV windows.

```bash
sudo boot/install-gpubundle.sh
```
This puts the plugin into the Data volume.

The persistent shader cache needs nothing at install time: the driver fills it in the DRV window on the first boots. To pre-fill it on the host, see `boot/macos-gpu/hostcache`.

## 6. Kernel and boot image

Build the kernel with the touch and GPU sources linked into the NativeUfs bundle, using ForSheng's recipe (`boot/MACOS-RETURN.md` step 7):

```bash
cd ~/uke/refs/macOS-ForSheng
UKE_TOUCH_SRC=~/uke/boot/macos-touch/src UKE_GPU_SRC=~/uke/boot/macos-gpu/src UKE_DRV_CONFIG=~/uke/boot/macos-gpu/drv.json \
  .venv/bin/python scripts/build_macos26_xnu.py --link-kernel --build-ufs-driver --build-gop-display --arch arm64e \
  --kdk-root ~/uke/ipsw/macos26/kdk/expanded/KDK.pkg/Payload -j 10
.venv/bin/python scripts/emit_macos26_security.py --kernel-build out/macos26-xnu-arm64e --output out/uke-prelink \
  --qemu-only --recovery-components --hid-components --include-module com.apple.iokit.IOMobileGraphicsFamily \
  --include-module com.apple.iokit.IOSurface --include-module com.apple.iokit.IOAVFamily \
  --storage-driver out/macos26-xnu-arm64e/ufs-driver
```

`UKE_GPU_FIRMWARE` is `drv` by default: the firmware is read from the DRV window at boot. Use `embed` to link it into the kernel instead (about 150 KiB more).

Then pack the kernel into `vendor_boot`. These are the boot arguments the author's current image uses:

```bash
cd ~/uke
CPUS=8 FULL=1 GOP=1 SIP0=0x7f TRUSTCACHE=boot/out/full-os-drv.trustcache boot/make-macos-image.sh \
  native-ufs-isc-overlay=1 native-ufs-isc-overlay-mb=1024 native-ufs-os-overlay=2048 native-gop-display=1 \
  native-gop-scale=2 sheng-recovery-ui=1 native-ufs-macoslog=2 sheng-touch=4 uke-unsealed-root=1 \
  uke-critical-nopanic=1 armcpu-self-ipi=1 sheng-gpu-power=9 sheng-gpu-dcvs=1 uke-gpu-metal=1 uke-ui-skip=1 \
  sheng-fb-gpu=1 uke-metal-clients='*' sheng-cpufreq=3 sheng-gpu-owned-coherent=1 vm_compressor=2 sheng-data-persist=2
```

Make the trust cache with `make-drv.py trustcache ipsw/macos26/fullos/full-os.trustcache boot/out/drv-stage boot/out/full-os-drv.trustcache`. The base `full-os.trustcache` is a static trust cache of the full-OS binaries DarwinBoot hands to the kernel. The author built it on 2026-11-02, and its generator is not in this repository yet. The output is `boot/out/vendor_boot_uke_macos.img`.

## 7. Flashing

1. Boot OrangeFox from RAM, back up whatever you still need, and check the partition table with `sgdisk -p /dev/block/sda`.
2. Set up the macoslog FAT near the end of userdata (the retained kernel log):
   - back up the FAT image;
   - make the snapshot file inside it with ForSheng's `scripts/prepare_native_log_file.py` and `install_native_log_file.py` (`config/macoslog.json`; the author's image went at 4 KiB block 30693371);
   - from OrangeFox, run `boot/macos-cycle.sh guard 512k` to write the guard block.

   All the LBAs (`LOG_LBA`, `GUARD_LBA` in `macos-cycle.sh`) are for a 128 GB unit.
3. In fastboot:
   - flash `boot_a` with `boot/out/boot.img` and `vendor_boot_a` with the new image;
   - stream the system into userdata with `python3 -I boot/macos-full-root.py flash`;
   - run `fastboot set_active a`.

   **Always flash boot_a and vendor_boot_a together**: a vendor_boot alone lands in fastboot. `boot/macos-cycle.sh flash <image>` does both.
4. From OrangeFox, write `drv.img` at userdata + 23735 MiB, and `manifest-drv.bin` at LBA 0 of userdata (4 KiB).
5. Reboot. The first boot takes several minutes.

## 8. Test loop and logs

- `boot/macos-cycle.sh pull NAME`: run it with the tablet in fastboot or OrangeFox. It boots the ROM to OrangeFox, reads the retained 512 KiB log, decodes it and prints a summary (GPU `UKE HANG` / `UKE PERF` rows, shader cache hits, panics).
- `boot/macos-cycle.sh flash IMAGE`: flashes boot_a and vendor_boot_a.
- To change only a few files of the OS image, hash its 16 MiB chunks before and after, then write only the chunks that changed, at userdata + 2 MiB + i·16 MiB. Verify the OS window's SHA-256 against `full-root.json` afterwards.
- Prefer cold boots: a warm reboot can hang.

## Known problems

See the README: performance (fence waits), stray triangles in the window animations, a mosaic when rotating, warm-reboot hangs, and no battery or thermal governor yet.
