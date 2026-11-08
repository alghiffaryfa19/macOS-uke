# macOS-uke

Full macOS 26.5 (25F71) on the Xiaomi Pad 7 (`uke`, Snapdragon 7+ Gen 3 / SM7675). It is built on [CrossDarwin/macOS-ForSheng](https://github.com/CrossDarwin/macOS-ForSheng) and [UniXNU](https://github.com/UniXNU/UniXNU), and on Allenwdk's iPadOS 27 GPU work for uke ([Allenwdk/UniXNU-uke](https://github.com/Allenwdk/UniXNU-uke)).

This repository records the work as of 2026-11-08. It is unfinished research. **To build and install it, follow [INSTALL.md](INSTALL.md).**

## Status

Working on the tablet:

- The ForSheng EL1 kernel boots on all 8 cores (PSCI and IPIs).
- The full macOS root, not the recovery one. The system volume is unsealed; the Data, Preboot and VM volumes are added by hand.
- WindowServer and loginwindow are stable.
- **Adreno 732 GPU with Metal**: the Adreno740 kernel driver, plus the AdrenoMetal plugin with the metal2vulkan translator and Turnip. WindowServer composites on the GPU (`-[MTLDevice acceleratorPort]` in the plugin), with blur and transparency.
- **Desktop** with the Finder menu bar, the Dock, the wallpaper, status icons, the clock, the Macintosh HD icon and widgets.
- **Persistent shader cache** in the DRV window: `ukecache` in `gpu/src/macos/UkeGpu.cpp`, plugin methods 0x7100/0x7101, and FUA writes in NativeUfs. On the second boot, WindowServer finds about 107 of its 209 pipelines in the cache.
- User `uke` with automatic login and no Setup Assistant.
- Novatek NT36532 touch as an absolute pointer (`UkeTouchPointer`).
- A 512 KiB retained log in the `macoslog` FAT (`scripts/macos-cycle.sh pull`), with `UKE HANG` and `UKE PERF` dumps from the GPU driver.

Key GPU fixes (in `gpu/patches/`):

- `0008`: freshly zeroed write-combined pages are cleaned and invalidated in the cache. This was the cause of the hangs on zero words in the IBs.
- `0007`: jobs of the guilty process are not replayed after a recovery, which stops the reboot loop.
- `0009`: the expensive IB checks are off by default (`uke-gpu-ib-check`, `uke-gpu-ib-publish`).
- `gpu/turnip/0001`: whole command buffers are submitted on Darwin.

Still broken:

- **Performance**: there are many fence-wait timeouts while the GPU is mostly idle. This is the next thing being investigated.
- A mosaic when rotating the tablet.
- Occasional hangs on warm reboot; cold boots are fine.
- No battery reading on macOS yet (the iPadOS SPMI ADC driver is still to be ported), and no thermal CPU governor.
- No HiDPI scaling; `native-gop-scale=2` is used instead.

## Contents

| Path | What it holds |
| --- | --- |
| `INSTALL.md` | Building and installing from scratch: inputs, workspace, UEFI, system image, GPU, kernel, flashing |
| `setup-workspace.sh` | Rebuilds the development workspace: public repos at their base commits plus `patches/` |
| `scripts/` | Test cycle (`macos-cycle.sh`: log pull, boot_a + vendor_boot flashing, log guard blocks); GPU bundle install (`install-gpubundle.sh`); UEFI `boot.img` (`make-boot-uefi.sh`); kernel build; the `vendor_boot` image (`make-macos-image.sh`); the full APFS container (`fullos-volumes.sh`, `fullos-populate.sh`, `apfs-set-volume-group.py`); no-Setup-Assistant configuration (`macos-skip-setup.py`); userdata flashing composed on the fly (`macos-full-root.py`) |
| `docs/` | `MACOS-RETURN.md` (rebuilding full macOS on the author's tablet) and `METAL-MACOS-PLAN.md` (GPU and Metal); both in Spanish |
| `gpu/` | The GPU port: source staging, patches, kernel glue (`src/macos`), the Metal plugin and Turnip patches, the DRV window, and host-side shader cache priming (`hostcache/`) |
| `touch/` | The macOS touch driver (GENI SPI and Novatek), without firmware |
| `patches/forsheng/` | Changes to macOS-ForSheng (base `ca57db7`, branch 26.5): commits, uncommitted changes and new files |
| `patches/unixnu/`, `patches/unixnu-sheng/` | Changes to the UniXNU and UniXNU-sheng checkouts inside ForSheng and their submodules (ArmCpu, DarwinBoot, NativeUfs…) |
| `patches/uefi*/` | The UniXNU UEFI for `boot_a`: uefi-uke (with mu_basecore), the UniXNU-uke device tree and the UniXNU core with DarwinBoot |
| `patches/device/` | `make-uke-adt.py` and `patch-vendor-boot.py` from Allenwdk/UniXNU-uke, as used by `make-macos-image.sh` |
| `patches/a27-*/` | The iPadOS 27 GPU baseline the macOS driver is staged from: Allenwdk/UniXNU (+ ArmCpu), Allenwdk/UniXNU-uke and Allenwdk/uefi-uke, with the author's unit settings |
| `maint/export-patches.sh` | Maintainer tool that refreshes `patches/` from the working trees |

Each `patches/*/BASE.txt` names the public repository and the commit the patches apply to.

## What is NOT here

Not published. Everyone has to obtain these on their own (see INSTALL.md, step 0):

- the GPU firmware (gen71100: GMU, SQE and ZAP) and the touch firmware (Tianma or CSOT), taken from your own tablet;
- Apple software: the IPSW, dyld cache, cryptexes, the KDK and private headers;
- your unit's unique chip ID (`chip-id.txt`), any keys, and generated images;
- the sources `gpu/stage.py` copies from the iPadOS 27 tree (regenerate them with that script).

## Lessons learned

- **Dock and menu bar:** when you skip Setup Assistant, also write the `com.apple.SetupAssistant` version keys (`LastSeenBuddyBuildVersion`, `PreviousBuildVersion`…) and `OptimizerPreviousBuild` in `loginwindow`. Without them, loginwindow treats the boot as an update and waits for MiniBuddy. See `scripts/macos-skip-setup.py`.
- **Cryptex without ignition:** the dyld cache must be in `/System/Library/dyld`, and the cryptex's loose libraries at their real paths on the system volume.
- **Metal and SIP:** Metal only loads plugins that SIP trusts. With `lp-sip0=0` it ignores the plugin; it needs `SIP0=0x7f`.
- **WindowServer on the GPU:** SkyLight chooses its compositor from `-[MTLDevice acceleratorPort]`. Without it, WindowServer falls back to CPU compositing even though Metal works.
- **WC memory:** zero-filling write-combined pages through a cached alias leaves dirty lines that are written back later over GPU data. Clean and invalidate them before handing the pages out.
- **Flashing:** always flash `boot_a` and `vendor_boot_a` together. A vendor_boot alone lands in fastboot.
- **Touch:** always build with `UKE_TOUCH_SRC` exported, and check that `UkeTouchPointer` is in the prelink.

The tablet's test account is `uke`, password `uke` (local only).

The patches to ForSheng, UniXNU and the other projects keep those projects' licenses.
