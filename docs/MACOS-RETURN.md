# Volver a macOS completo en uke (solo macOS, se borra Windows)

Decidido por el usuario el 2026-11-06: "vamos a montar solo macOS". Windows 11, la partición WINPE y la partición `win` se borran de la tablet.

## En el Mac

1. Descargar el sistema 25F71 (`094-56453-087.dmg`) y el cryptex SystemOS (`094-56679-089.dmg`) a `ipsw/macos26/fullos/25F71__MacOS`, con `ipsw extract --remote`.
   - **`ipsw extract` no descifra:** después hay que ejecutar `ipsw fw aea <x>.dmg.aea -o .`. Para el cryptex se usa `--pem ../../keys/25F71__MacOS/094-56679-089.dmg.aea.pem`.
   - Borrar el `.aea` solo cuando el `.dmg` exista. El sistema descifrado mide 13071548416 B.
   - El log queda en `ipsw/macos26/dl2-2026-11-06.log`.
2. Descargar el KDK 25F71 a `ipsw/macos26/kdk` y expandirlo en `kdk/expanded`. Hacerlo después de borrar el cryptex, porque no hay espacio para todo a la vez.
3. Generar la ISC vacía con `refs/macOS-ForSheng/scripts/prepare_qemu_isc.py --output <dir nuevo>` y mover `isc.raw` a `ipsw/macos26/fullos/isc-window.img`. Su sha va a `digests.ISC` de `full-root.json`; `macos-full-root.py plan` lo conserva. También después del cryptex, por espacio.
4. `boot/fullos-volumes.sh`, sin sudo:
   - amplía la imagen a 22735224832 B, el mismo tamaño que antes, para que no se muevan las ventanas ISC y DRV;
   - añade los volúmenes Data, Preboot y VM;
   - guarda el UUID de Data en `ipsw/macos26/fullos/data-uuid.txt`;
   - agrupa Data con System.
5. **`sudo boot/fullos-populate.sh`, una sola vez, lo ejecuta el usuario.** Hace esto:
   - Data a partir de Templates/Data;
   - caché dyld en System;
   - el resto del cryptex en Preboot y en System, sin sobrescribir;
   - sin asistente de configuración (usuario `uke`);
   - enlace `/Library/GPUBundles` y línea de fstab para UKEDRV.
6. Calcular los digests con `python3 -I boot/macos-full-root.py plan`, que da los nuevos OS y ISC.
7. Poner los digests de OS e ISC en ForSheng, en `config/macos26-recovery-root.json`, y recompilar el kernel con la receta de `_8c_home`:
   - `build_macos26_xnu.py --link-kernel --build-ufs-driver --build-gop-display --arch arm64e --kdk-root …/kdk/expanded/KDK.pkg/Payload -j 10`;
   - después `emit_macos26_security.py --kernel-build out/macos26-xnu-arm64e --output out/uke-prelink --qemu-only --recovery-components --hid-components --include-module com.apple.iokit.IOMobileGraphicsFamily --include-module com.apple.iokit.IOSurface --include-module com.apple.iokit.IOAVFamily --storage-driver out/macos26-xnu-arm64e/ufs-driver`.
8. Crear la imagen de arranque con los mismos argumentos que `_8c_home`:
   ```
   CPUS=8 TRUSTCACHE=…/full-os.trustcache FULL=1 GOP=1 boot/make-macos-image.sh native-ufs-isc-overlay=1 native-ufs-isc-overlay-mb=1024 native-ufs-os-overlay=2048 native-gop-display=1 native-gop-scale=2 sheng-recovery-ui=1 native-ufs-macoslog=2 sheng-boot-progress=1 sheng-touch=4 uke-unsealed-root=1 uke-critical-nopanic=1 uke-crashinfo=0x1e9bb6c40 armcpu-self-ipi=1
   ```

## En la tablet (fastboot y OrangeFox)

1. Arrancar OrangeFox solo en RAM: `fastboot boot boot/ofox/orangefox-r12-uke-modded-v003.img`.
2. Restaurar la GPT de LUN0 anterior a Windows:
   ```
   adb push ~/Desktop/woa-uke/device_dumps/gpt-lun0-pre-windows-install/gpt-sda.sgdisk /tmp/
   adb shell sgdisk --load-backup=/tmp/gpt-sda.sgdisk /dev/block/sda
   ```
   Con eso vuelve `userdata` (entrada 32, 105,9 GiB, sectores 2961408..30709754) y `bk07` (entrada 22). Después, reiniciar OrangeFox o hacer `blockdev --rereadpt`, y comprobarlo con `sgdisk -p`.
3. Rehacer el FAT del log al final de userdata, porque Windows lo pisó:
   ```
   adb push boot/macoslog/prepared/macoslog.img /tmp/
   adb shell dd if=/tmp/macoslog.img of=/dev/block/sda bs=4096 seek=30693371
   ```
4. Volver a fastboot y flashear:
   - `boot_a` = `boot/out/boot.img`, la UEFI UniXNU con los bloques DarwinBoot de 32 MB;
   - `vendor_boot_a` = la imagen nueva;
   - userdata con `python3 -I boot/macos-full-root.py flash` (24,9 GB, en piezas dispersas de 512 MiB);
   - `fastboot set_active a`.
5. Para Metal (fase 1 y siguientes):
   - escribir `drv.img` y `manifest-drv.bin`, regenerados con los digests nuevos (`boot/macos-gpu/make-drv.py`), con `seek=23735` (MiB);
   - usar la imagen con los parches NativeUfs DRV, ArmCpu y GPU, y `sheng-gpu-power=7`.

No hay copia del boot y vendor_boot de la ROM en la tablet. Están en `woa-uke/device_dumps/backup-pre-windows` (SHA256SUMS).
