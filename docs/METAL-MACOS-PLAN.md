# Metal en macOS 26.5 para la uke (Adreno 732): plan por fases

Actualizado el 2026-11-06: la base ya no es la versión de iPadOS 17 (`metal_up2`), sino **nuestra build de iPadOS 27** (24A446, la que funciona en la tablet): driver Adreno740 22abfe4, AdrenoMetal 854f99a y el repo de dispositivo `UniXNU-uke-allenwdk` (rama `uke-a27-local`, nuestro pin de zap). Lo de iPadOS 17 queda en `patches/ipados17/` y `plugin/ipados17/` solo como referencia. Nada de esto se ha probado todavía en la tablet. Todo lo nuevo está en `boot/macos-gpu/`. No se ha hecho commit, no se ha tocado `refs/macOS-ForSheng/out/`, `config/`, `platform/sheng/macos26/patches/` ni `scripts/build_macos26_xnu.py`, y los parches para ForSheng y NativeUfs están preparados pero **sin aplicar**.

## Resumen

| Fase | Estado | Qué falta para probar |
| --- | --- | --- |
| 1. Kernel: encender la GPU y cargar el firmware | Base iPadOS 27: compila con la receta de XNU de macOS (48 objetos, 438 importaciones, ninguna weak). Cabe en `vendor_boot` (+640 KiB con firmware embebido). | Aplicar los parches a ForSheng (GPU y, recomendado, ArmCpu), compilar y arrancar con `sheng-gpu-power=7` y luego `=9`. |
| 2. Plugin Metal | Base iPadOS 27: bundle completo compilado y firmado el 2026-11-06 (plugin 18,6 MB + Turnip 17,6 MB, arm64e, macOS 26.5, CDHash 9421db53…). Páginas de 16 KiB portadas (compila, sin probar). Clase `IOAccelerator` del kernel lista. No se ha cargado nunca en un WindowServer. | Poner el enlace en `/System/Library/Extensions` y quitar las reglas de interfaz que fuerzan el software. |
| 3. Ventana DRV | Parche de NativeUfs compilado. `drv.img`, manifiestos y trust cache ya generados en `boot/out` con el bundle nuevo. | Aplicar el parche de NativeUfs, compilar el kernel y hacer `dd` desde OrangeFox. |

## Archivos nuevos

- **Preparación de fuentes**
  - `boot/macos-gpu/stage.py` copia a `boot/macos-gpu/src/` exactamente lo que compiló nuestra build de iPadOS 27 para la GPU, desde su árbol generado `UniXNU-a27/out/uke/ipados-27.0.1-24A446/uke_ipados27/tree` (`<tree>`):
    - `src/drivers/Adreno740`: el driver (`<tree>/drivers/Adreno740` → `UniXNU-a27`, 22abfe4);
    - `src/package`: `GpuPowerProbe.cpp`, `GpuPowerBoard.h`, `ShengGmuVotes.*` y `BootLatencyTrace.h` de `UniXNU-uke-allenwdk/package`;
    - `src/soc`: `ShengGmu.*` y `ShengGmuCommandDb.*` del feed sm7675;
    - los kits `AdrenoFirmware`, `ArmSmmu4K` y `AdrenoPM4`;
    - las cabeceras GMU y SMMU tal como las generó esa build (`<tree>/build/sheng-gmu`, `sheng-gpu-smmu`);
    - `firmware.S` con el firmware `gen71100` de nuestros pines (es privado: no subirlo nunca).
  - Comprueba que `UniXNU-uke-allenwdk` está en `uke-a27-local`, aplica `boot/macos-gpu/patches/000N-*.patch` y deja el registro en `stage.json`. Necesita Python 3.11 o superior: `refs/macOS-ForSheng/.venv/bin/python -I boot/macos-gpu/stage.py`.
- **Parches sobre las copias de iPadOS**
  - `patches/0001-gpu-power-macos-kernel-imports.patch`: quita `weak` a los símbolos que el kernel de macOS sí define (`armcpu_spi_attach`, `armcpu_spi_unhandled`, contadores de IPI, `vm_page_free_count`). El prelinker de ForSheng rechaza las importaciones weak, igual que pasó con el touch.
  - `patches/0002-adreno740-16k-pages.patch`: páginas de 16 KiB del kernel de macOS (ver la fase 2).
  - El parche del command DB en el heap ya no hace falta: la versión de iPadOS 27 ya lo trae así.
- **Código nuestro para macOS**
  - `src/macos/UkeGpu.cpp`:
    - Servicio IOKit `UkeGpu` sobre IOResources, como `UkeTouchPointer`. Arranca `sheng_gpu_power_probe()` y vuelca la etapa de la sonda al log cada 30 s.
    - Define los símbolos que solo existen en iPadOS: progreso de arranque, traza de latencia (apagada), estado del addon, `boot_log_export_put`, `native_pmap_user_mappings` (diagnóstico), accelerator de iOS, `proc_iterate` y el handoff GOP.
    - `native_hid_input_events` (los toques, que la sonda usa para subir la frecuencia de la GPU) lo define nuestro driver de touch de macOS en el mismo bundle, así que `UKE_GPU_SRC` exige `UKE_TOUCH_SRC`.
    - En modo DRV lee el firmware de la ventana DRV y comprueba su SHA-256.
    - Con `uke-gpu-metal=1` publica el accelerator.
  - `src/macos/UkeGpuFirmwareRemap.h`: en modo DRV convierte los arrays del firmware en punteros, sin tocar las fuentes de iPadOS.
  - `src/macos/UkeGpuAccelerator.cpp` (fase 2): clase `AGXAccelerator : IOAccelerator`, que reenvía a `native_adreno_new_user_client` (el user client `AGXDeviceUserClient` que ya usa el plugin).
- **Compilación y medida**
  - `boot/macos-gpu/uke_gpu_build.py`:
    - Lo llama `build_macos26_storage.py` mediante `UKE_GPU_SRC`.
    - `UKE_GPU_FIRMWARE=embed|drv|none`, por defecto `drv`.
    - `UKE_GPU_IOACCELERATOR=own|family`, por defecto `own`.
    - Por sí solo (`--out DIR`) compila todo, enlaza un kext de prueba y comprueba las importaciones contra `kernel.development.sheng`.
  - `boot/macos-gpu/measure.py DIR`: dice cuánto crece el kernel prelinkado frente a la FAT actual.
- **Integración en ForSheng**
  - `boot/macos-gpu/forsheng/forsheng-drv-gpu.patch`, para `refs/macOS-ForSheng`:
    - `build_macos26_storage.py`: objetos de la GPU, la personalidad `UkeGpu` y los defines de DRV leídos de `UKE_DRV_CONFIG`;
    - `native_storage_module.py`: admite la personalidad `UkeGpu`;
    - `recovery_root.h`: identidad de la ventana DRV.
  - `boot/macos-gpu/forsheng/nativeufs-drv-window.patch`, para `refs/macOS-ForSheng/UniXNU/drivers/NativeUfs`: tercera ventana `DRV` y `native_ufs_drv_read`.
  - `boot/macos-gpu/forsheng/armcpu-hypervisor-sgi.patch`, para `refs/macOS-ForSheng/UniXNU/drivers/ArmCpu`: nuestro arreglo de iPadOS 27 (`UniXNU-a27/drivers/ArmCpu` 940b493 + f0aa510). Acepta las SGI/PPI que lanza el hipervisor Gunyah, en lugar de hacer `panic("ArmCpu: unexpected architectural IRQ")`; el ArmCpu de ForSheng todavía hace ese panic. Compila con la receta de macOS y `kprintf` está en el kernel. **Sin aplicar:** ForSheng es de la sesión principal y tiene cambios sin commit.
    ```bash
    git -C refs/macOS-ForSheng/UniXNU/drivers/ArmCpu apply ../../../../../boot/macos-gpu/forsheng/armcpu-hypervisor-sgi.patch
    ```
- **Volumen DRV**
  - `boot/macos-gpu/drv.json`: contrato de la ventana DRV.
  - `boot/macos-gpu/make-drv.py`: órdenes `image`, `manifest [without-drv]`, `trustcache` y `verify`.
- **Plugin para macOS**
  - `boot/macos-gpu/build-turnip-macos.py`: Turnip arm64e para macOS 26.5, con la misma receta que `--target ios27` (Mesa fijada y parcheada de `<tree>/.cache/mesa-work`, chained fixups, `tu-build-id`).
  - `boot/macos-gpu/build-plugin-macos.py`: puente Rust, plugin, frontera PAC, bundle firmado y preparación para DRV. Ejecuta el `build-adreno-metal-bridge.py` de `<tree>` con un target `macos` añadido en memoria, sin tocar el repo.
  - `boot/macos-gpu/plugin/0001-macos-windowserver.patch`: el plugin para macOS, sobre AdrenoMetal 854f99a.

## Fase 1: kernel

### Qué cambia respecto a iPadOS
- **IOKit:** NativeAdreno no usa IOAccelerator ni IOGPU en el camino de encendido, solo IOService, IOUserClient e IODeviceMemory. Compila contra las cabeceras del kernel de macOS sin cambios de API.
- **Arranque de la sonda:** en iPadOS la arrancaba `LegacyDisplayProbe`. En macOS no existe, así que lo hace `UkeGpu`.
- **Carga en la colección:** el kernel de ForSheng no enlaza archivos `.a` de drivers. Los objetos van dentro del bundle NativeUfs, que ForSheng prelinka como `__NVB_*`, igual que el touch.
- **Resolución de símbolos:** el prelinker solo acepta importaciones no weak que existan en el kernel. Comprobado con la base de iPadOS 27: 48 objetos, las 438 importaciones resuelven y no hay ninguna weak.
- **Memoria de la GMU:** el ADT de macOS (`make-uke-adt.py` de ios26) ya lleva el nodo `gmu-memory`. El UEFI reserva `0xC8000000` y el kernel de macOS solo gestiona RAM desde `0x8C0000000`.
- **Páginas de 16 KiB:** el kernel de macOS las usa. Los modos 1–9 no se ven afectados porque usan el pool reservado. `NativeAdrenoQueryCpu4K` devuelve `Unsupported`, así que los BO de usuario y la sonda de coherencia se rechazan de forma segura, sin marcar páginas equivocadas (ver la fase 2).
- **Accelerator de iOS:** `publishMetal` (`sheng-gpu-metal=1`) busca `IOAcceleratorES`, que no existe en macOS. **No usar `sheng-gpu-metal`.**

### Tamaño
Medido el 2026-11-06 con `measure.py` (base de iPadOS 27) frente a la FAT de la última imagen de macOS (`vendor_boot_uke_macos_full_8c_home.img`), que tiene 991.232 B libres.

| Variante | Crece el kernel | ¿Cabe? |
| --- | --- | --- |
| Firmware embebido (`UKE_GPU_FIRMWARE=embed`) | +640 KiB | Sí, ~330 KiB de margen |
| Firmware en DRV (`drv`) | +480 KiB | Sí, ~490 KiB de margen |

- **Por qué cuenta el bss:** el prelinker de ForSheng copia el bundle con su tamaño virtual completo (bss y `__LINKEDIT` incluidos). Por eso el command DB va en el heap.
- **Margen extra si hace falta:** el `vendor_boot` admite 200.704 B más. Se pueden usar cambiando en `make-macos-image.sh` `-T $((FAT_MIB*2048))` por `-T $((94*2048+384))`.

### Pasos para integrarlo (primera prueba, con firmware embebido)
1. Aplicar el parche en ForSheng:
   ```bash
   cd refs/macOS-ForSheng && git apply ../../boot/macos-gpu/forsheng/forsheng-drv-gpu.patch
   ```
2. Preparar las fuentes:
   ```bash
   refs/macOS-ForSheng/.venv/bin/python -I boot/macos-gpu/stage.py
   ```
3. Compilar con vuestro mismo comando de `build_macos26_xnu.py --link-kernel --build-ufs-driver …`, añadiendo estas variables:
   ```bash
   UKE_TOUCH_SRC=$PWD/boot/macos-touch/src UKE_GPU_SRC=$PWD/boot/macos-gpu/src UKE_GPU_FIRMWARE=embed
   ```
   Luego `emit_macos26_security.py` como siempre. El bundle debe llevar las personalidades `UkeGpu` y `UkeTouchPointer`.
4. Crear la imagen con `make-macos-image.sh` (los mismos argumentos de `_8c_ipi1_ov`/`nosetup`) añadiendo los argumentos de arranque `sheng-gpu-power=7 sheng-gpu-coherent=0`.
   - Si `make-macos-image.sh` dice "kernel did not fit", ampliar la FAT como se indica arriba o usar el firmware en DRV.
5. Si sale bien, probar con `sheng-gpu-power=9` y opcionalmente `sheng-gpu-perf=7` (835 MHz en iPadOS).

### Qué buscar en macoslog
- `UkeGpu: starting the Adreno 732 bring-up, sheng-gpu-power=N`
- `UKE GPU1..4:`: las mismas filas que en iPadOS (tabla de `GPU-PLAN.md`):
  - modo 7: `G1/0/0/0>G0/1/0/1` (GMU arrancada y HFI respondiendo);
  - modo 9: `C10ok`, `S zap`, `S irq`, `P published`.
- `UKE GPU: stage 0x..` cada 30 s, para ver dónde se queda si se cuelga.

## Fase 2: plugin Metal

### Cómo descubre Metal la GPU en macOS
Trazado con lldb en este Mac: `MTLRegisterDevices` llama a `-[MTLIOAccelServiceGlobalContext init]`, que busca `IOServiceMatching("IOAccelerator")`. Lee `MetalPluginName` y `MetalPluginClassName`, carga `/System/Library/Extensions/<MetalPluginName>.bundle` y llama a `initWithAcceleratorPort:`. Antes consulta `IOSurfaceRoot`, que existe porque IOSurface está prelinkado.

### Kernel
- `UkeGpuAccelerator.cpp` publica `AGXAccelerator` con `MetalPluginName=NativeAdrenoMetal` y `MetalPluginClassName=NativeAdrenoMetalDevice`.
- Se activa con el argumento de arranque `uke-gpu-metal=1`, cuando la sonda ya ha publicado `NativeAdrenoGpu` (modo 9).
- **`IOAccelerator` vive en IOGraphicsFamily**, que no está en la colección de arranque de ForSheng (~350 KiB, no cabe). Por eso, por defecto, el bundle define su propia clase vacía `IOAccelerator`. Esto solo vale mientras IOGraphicsFamily no se cargue nunca. La alternativa (`UKE_GPU_IOACCELERATOR=family`) exige añadir IOGraphicsFamily a la colección.

### Userland
- **arm64e:** WindowServer y todos los procesos de plataforma son arm64e. La versión de iPadOS 27 ya era arm64e (su render server también lo es), así que la receta se reutiliza casi entera; solo cambian el SDK (macosx), el target (`arm64e-apple-macos26.5` / `arm64e-apple-darwin`) y lo que nombra procesos o clases de iOS.
  - **Turnip:** `build-turnip-macos.py` → `<tree>/build/mesa-tu-macos` (18,4 MB, arm64e, plataforma macOS, minos 26.5). ~5 min.
  - **Plugin y frontend:** compilan con `-Werror` en arm64e con `plugin/0001-macos-windowserver.patch`, que añade `UkeRenderServer.h` y:
    - cambia el proceso dueño de backboardd a WindowServer (plugin y frontend);
    - cambia la clase de `IOAcceleratorES` a `IOAccelerator`, también para Turnip (`TU_DARWIN_SERVICE`, cuyo valor por defecto es el de iOS);
    - cambia las cachés de `/var/mobile` y `com.apple.backboardd`;
    - **excluye en macOS el bloque `__arm64e__` que escribe en direcciones fijas de QuartzCore de iPadOS 27** (24A446). En macOS, al ser también arm64e, se habría compilado y corrompido memoria de WindowServer.
  - **Puente metal2vulkan (Rust):** `arm64e-apple-darwin` no tiene `std` precompilada; se compila desde `rust-src` (ya instalado en 1.98.0) con `-Z build-std`, como `ios27`. Mismo pin del traductor, mismos 30 parches y mismo runtime id que iPadOS 27.
  - **Frontera PAC:** como en iPadOS 27, las llamadas del código Rust que reciben callbacks pasan por `RustPacBoundary.c` y sus saltos indirectos por `rust-pac-rewrite.py` tras el enlace.
  - Orden completa (puente + enlace + firma):
    ```bash
    python3 -I boot/macos-gpu/build-plugin-macos.py --stage <carpeta-DRV>
    ```
    Usa `--publish` cuando se quiera que WindowServer use de verdad el dispositivo nativo. Sin `--publish`, el plugin solo corre la prueba de la etapa 1 y devuelve nil.
  - No se incluye la lista de metallibs para precompilar (`prime-list.txt`): la de iPadOS 27 nombra rutas de iOS. Habría que generarla desde el sistema de macOS.
- **Referencias:** `AdrenoMetal-upstream` es la versión de iPadOS (sheng), no de macOS. La de macOS que funciona es Snapintosh (submódulos SnapAdreno, SnapMetal y air2ir3 clonados en `refs/Snapintosh/gpu` el 2026-11-06). Implementa todo el protocolo IOGPU de Apple (`AdrenoMTLDevice : IOGPUMetalDevice`, kext IOGPU desde la colección auxiliar y pantalla IOMobileFramebuffer propia). Nuestro camino (`_MTLDevice` + user client propio) es el que funcionó en iPadOS; que SkyLight lo acepte en macOS es lo que hay que comprobar.
- **Lo que Snapintosh aprendió de WindowServer en macOS y nos sirve como lista de comprobación en la prueba 2b:**
  - WindowServer espera 30 s a un dispositivo Metal y aborta ("No suitable Metal devices present after 30 seconds") si no tiene política de software; ForSheng ya la tiene.
  - `MTLAddDevice` exige conformidad con `MTLDevice` y `MTLDeviceSPI`. Lo cumplimos: el protocolo existe en el runtime de macOS (comprobado) y el frontend lo añade.
  - La discovery llama a `_setAcceleratorService:`, que en macOS solo implementa `IOGPUMetalDevice`, no `_MTLDevice`. Nuestro frontend responde con cero a cualquier selector desconocido, así que no rompe.
  - `initLimits` aborta si los límites que salen de `featureProfile` suman 0. Nuestro frontend lo deja vacío a propósito.
  - `vendorName` no puede contener "intel" (QuartzCore toma caminos de Intel).
  - `newTextureWithDescriptor:iosurface:plane:` y `newSamplerStateWithDescriptor:` nunca pueden devolver nil: WindowServer aborta.
  - Sin lectura del framebuffer (framebuffer fetch), las esquinas redondeadas, el desenfoque y los fondos salen negros. Si los kernels de compute fallan, el filtro APL deja la salida negra; `CA_DISABLE_APL_FILTER` es el atajo.
  - Responder `iosurfaceTextureAlignmentBytes` hizo abortar el desenfoque de CoreImage en su caso.
  - WindowServer crea a veces búferes de cliente en direcciones no alineadas a página (Ajustes > Pantallas); ellos los mapean en páginas completas. Nuestro `importBo` exige 4 KiB: vigilarlo en el log.

### Lo que falta en la fase 2, por orden
1. **BO con páginas de 16 KiB: hecho el 2026-11-06, compilado, sin probar** (`patches/0002-adreno740-16k-pages.patch`). La GPU sigue mapeando en trozos de 4 KiB exactamente lo que pide Turnip (`bytes()` y los spans no cambian). El lado CPU trabaja en páginas completas del kernel (`page_size`):
   - `NativeAdrenoQueryCpu4K` acepta el granulo de 16 KiB (TG0=2, TG1=1) además del de 4 KiB;
   - los números de página del pmap son `pa / page_size` (antes `pa>>12`) en `SetPagesCacheMode`, `Reassert`, `PagesUncached` y el diagnóstico de la sonda;
   - el cambio de caché se aplica una vez por página del kernel que tocan los spans;
   - las reservas (`allocate`, las de sistema y las contiguas) piden páginas completas y alineadas, para que write-combined nunca afecte a memoria vecina;
   - el mailbox que se mapea en el render server ocupa una página completa del kernel (antes eran 4 KiB, que podían compartir página con otros datos del kernel).
   - Los imports (memoria del propio proceso) y las tablas de la SMMU no cambian: ya funcionan con direcciones de 4 KiB.
2. **Dónde busca Metal el plugin y SIP** (comprobado el 2026-11-06 desensamblando `libsystem_sandbox` en este Mac, macOS 27 26A428, la misma build que documenta Snapintosh; en 25F71 está sin comprobar):
   - `gpu_bundle_find_trusted` prueba, en este orden, `/Library/GPUBundles/<MetalPluginName>.bundle` y `/System/Library/Extensions/<MetalPluginName>.bundle` (la tercera ruta, `/AppleInternal/...`, solo vale en builds internas).
   - La ruta tiene que ser una carpeta según `stat`, que **sigue enlaces**: un enlace a `/Volumes/UKEDRV/NativeAdrenoMetal.bundle` vale.
   - Después exige que sea de confianza (`gpu_bundle_is_path_trusted` → `rootless_check_trusted_class(path, "KernelExtensionManagement")`, o que esté en la caché compartida de dyld). `rootless_check_trusted_internal` empieza por `csr_check(0x2)` (CSR_ALLOW_UNRESTRICTED_FS): **si SIP permite el sistema de archivos sin restricciones, cualquier ruta es de confianza**. Si no, solo lo está lo que esté en un volumen raíz autenticado o lo que tenga marcas de SIP.
   - **Consecuencia:** las imágenes de ForSheng arrancan por defecto con `lp-sip0=0` (SIP activo) y el sistema es un volumen sin sellar, así que **Metal ignoraría nuestro plugin** en cualquier ruta que añadamos. Para la fase 2 la imagen tiene que hacerse con `SIP0=0x7f` ("csrutil disable", que incluye 0x2; ya existe la variante `_8c_nosetup_sip`).
   - Con SIP desactivado, lo más simple es **`/Library/GPUBundles/NativeAdrenoMetal.bundle` → `/Volumes/UKEDRV/NativeAdrenoMetal.bundle`** en el volumen Data, en lugar de tocar `/System/Library/Extensions`. Como el Data de la tablet es una capa en RAM sobre la imagen flasheada, el enlace tiene que ir en la imagen base (como hizo `macos-skip-setup.py`), así que también hay que reflashear userdata y actualizar el digest en `config/` (lo hace la sesión principal). Además hace falta una entrada en fstab para que DRV se monte antes de WindowServer (por ejemplo `LABEL=UKEDRV /Volumes/UKEDRV hfs ro,nobrowse`).
3. **Trust cache: generada el 2026-11-06** en `boot/out/full-os-drv.trustcache` (15.326 entradas, 2 nuestras: plugin `9421db53…` y Turnip `037e3179…`). Si se recompila el plugin hay que regenerarla:
   ```bash
   python3 -I boot/macos-gpu/make-drv.py trustcache ipsw/macos26/fullos/full-os.trustcache boot/out/drv-stage boot/out/full-os-drv.trustcache
   ```
   Después pasarla con `TRUSTCACHE=boot/out/full-os-drv.trustcache`. Añade 24 B por binario.
4. **Prueba 2a, sin tocar el compositor:** `uke-gpu-metal=1` y el plugin sin `.publish`. Los marcadores de la etapa 1 (IOServiceOpen tipo `0x4147xxxx`) aparecen en el log si WindowServer carga el plugin.
5. **Prueba 2b:** con `.publish`, y quitando de la política de interfaz las reglas 0, 1 y 4 (Metal bloqueado, formato del framebuffer, BGRA), que fuerzan el compositor por software. Esto está en `config/`, que es de la sesión principal.
6. **Presentación en pantalla:** el escaneo directo de iPadOS (`native_gop_handoff`) se queda sin conectar. Las presentaciones de WindowServer siguen yendo a NativeGOPDisplay (copia por CPU).
7. **La caché de pipelines no sobrevive al reinicio**, porque el Data es una overlay en RAM. Solución: un pack precompilado `NativeAdrenoMetal.m2vpack` junto al bundle, en DRV.
8. **Lista de metallibs para precompilar (`NativeAdrenoMetal.primelist`):** no se ha hecho. Tiene que salir de la imagen del sistema 25F71, que ya no está en el disco (`ipsw/macos26/fullos/25F71__MacOS` está vacía), y este Mac tiene macOS 27, que no coincide. Es opcional: sin ella el plugin compila los shaders según los va necesitando.

## Fase 3: ventana DRV

### Formato (`drv.json`)
- 512 MiB justo después de ISC: offset 24.887.951.360 de userdata, es decir, el MiB 23735. El total del manifiesto pasa a 25.424.822.272.
- Bytes 0 a 511 MiB: volumen HFS+ `UKEDRV` sin mapa de particiones. macOS lo monta entero; comprobado en el Mac.
- Último MiB: tabla `UKEFW001` con los firmware gmu, sqe y zap, alineados a 4 KiB, cada uno con su SHA-256.

### Cambios en NativeUfs (`nativeufs-drv-window.patch` + `recovery_root.h`)
- Acepta 1–3 ventanas. El orden de publicación es ISC, DRV y luego OS.
- La identidad de DRV es su tamaño fijo más un digest todo a cero, porque se reescribe sin digest.
- La publicación de DRV no espera por su UUID, así que una DRV vacía o corrupta nunca bloquea el root.
- Sin overlay: es de solo lectura.
- Exporta `native_ufs_drv_read(offset, buf, len)` para que `UkeGpu` lea el firmware.
- Se compila solo si se define `NATIVE_UFS_DRV_BYTES`, que sale de `UKE_DRV_CONFIG`. El NativeUfs parcheado compila con la receta de ForSheng.

### Ya generado (2026-11-06)
- `boot/out/drv-stage/`: el bundle preparado para la prueba 2a (sin `.publish`).
- `boot/out/drv.img`: 512 MiB, HFS+ `UKEDRV` con el bundle (firma verificada montándolo) y la tabla de firmware (gmu, sqe y nuestro zap `73e07ec2`); `verify` OK.
- `boot/out/manifest-drv.bin` (sha256 `4db01592…`, 3 ventanas) y `boot/out/manifest-2w.bin` (sha256 `7193a321…`, las 2 de hoy, para volver atrás). Salen de `full-root.json` del 3-nov; si el OS se vuelve a flashear con otro digest, hay que regenerarlos.

### Pasos
1. Aplicar el parche de NativeUfs:
   ```bash
   git -C refs/macOS-ForSheng/UniXNU/drivers/NativeUfs apply ../../../../../boot/macos-gpu/forsheng/nativeufs-drv-window.patch
   ```
2. Compilar el kernel con estas variables añadidas:
   ```bash
   UKE_DRV_CONFIG=$PWD/boot/macos-gpu/drv.json UKE_GPU_FIRMWARE=drv
   ```
   **Flashear este kernel antes de cambiar el manifiesto:** el kernel actual rechaza 3 ventanas y daría panic.
3. Crear la imagen y el manifiesto. La imagen ocupa 512 MiB de disco del Mac.
   ```bash
   python3 -I boot/macos-gpu/make-drv.py image <carpeta-DRV> boot/out/drv.img
   ```
   ```bash
   python3 -I boot/macos-gpu/make-drv.py manifest boot/out/manifest-drv.bin
   ```
4. En OrangeFox:
   - Comprobar que `ls -l /dev/block/by-name/userdata` apunta a `sda32`.
   - Hacer `adb push` de los dos archivos a `/tmp`.
   - Ejecutar los dos `dd` que imprime `manifest`. El manifiesto es solo un bloque; la DRV va con `bs=1048576 seek=23735 conv=notrunc,fsync`.
   - **No se tocan las ventanas OS e ISC, ni el macoslog del final de userdata.**
5. Para volver atrás: `make-drv.py manifest boot/out/manifest-2w.bin without-drv` regenera el manifiesto actual de 2 ventanas; se hace `dd` del bloque 0 igual que antes.
6. Qué buscar en el log:
   - `NativeUfs: published DRV`;
   - `UkeGpu: DRV firmware gmu 80680 bytes ok` (y sqe 66036, zap 12088);
   - en macOS, `/Volumes/UKEDRV`. Si no se monta sin sesión iniciada: `AutomountDisksWithoutUserLogin`, o la entrada de fstab de la fase 2.
7. Para actualizar el plugin, Mesa o el firmware más adelante: solo `make-drv.py image` y el `dd` de la DRV.

## Disco del Mac
El 2026-11-02 el disco se llenó (ENOSPC). El 2026-11-06 había 13 GB libres y se volvió a compilar. Lo que ocupa la parte de macOS: Turnip `<tree>/build/mesa-tu-macos` (~185 MB), el target de Rust arm64e en `<tree>/build/m2v-target` (lo más grande) y `drv.img` (512 MiB). Todo se regenera con las órdenes de arriba. **No crear `drv.img` ni compilar el puente con menos de ~2 GB libres.**
