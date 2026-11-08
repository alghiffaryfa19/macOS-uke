# macOS-uke

macOS 26.5 (25F71) completo en la Xiaomi Pad 7 (`uke`, Snapdragon 7+ Gen 3 / SM7675), sobre el port [CrossDarwin/macOS-ForSheng](https://github.com/CrossDarwin/macOS-ForSheng) y [UniXNU](https://github.com/UniXNU/UniXNU).

Este repositorio recoge el trabajo en el estado del 2026-11-08. Es un trabajo de investigación sin terminar.

## Estado

Funciona en la tablet:

- Arranque del kernel EL1 de ForSheng con 8 núcleos (PSCI e IPI).
- Raíz completa de macOS (no la de recuperación). El volumen del sistema va sin sellar y los volúmenes Data/Preboot/VM se añaden a mano.
- WindowServer y loginwindow estables.
- **GPU Adreno 732 con Metal** (driver Adreno740 del kernel, plugin AdrenoMetal con el traductor metal2vulkan y Turnip). WindowServer compone por GPU (`-[MTLDevice acceleratorPort]` en el plugin), con desenfoque y transparencias.
- **Escritorio** con la barra de menús de Finder, el Dock, el fondo, los iconos de estado, el reloj, el icono de Macintosh HD y los widgets.
- **Caché de shaders persistente** en la ventana DRV (`ukecache` en `gpu/src/macos/UkeGpu.cpp`, método 0x7100/0x7101 del plugin y escritura FUA en NativeUfs). En el segundo arranque WindowServer acierta unos 107 de 209 pipelines.
- Usuario `uke` con inicio de sesión automático y sin asistente de configuración.
- Táctil Novatek NT36532 como puntero absoluto (`UkeTouchPointer`).
- Log retenido de 512 KiB en la FAT `macoslog` (`scripts/macos-cycle.sh pull`), con volcados `UKE HANG` y `UKE PERF` del driver de la GPU.

Arreglos importantes de la GPU (en `gpu/patches/`):

- `0008`: las páginas WC recién puestas a cero se limpian e invalidan en caché. Era la causa de los cuelgues por palabras a cero en los IB.
- `0007`: no se repiten los trabajos del proceso culpable tras una recuperación (evita el bucle de reinicios).
- `0009`: las comprobaciones caras de los IB quedan desactivadas por defecto (`uke-gpu-ib-check`, `uke-gpu-ib-publish`).
- `gpu/turnip/0001`: envíos de command buffer completos en Darwin.

Falla todavía:

- El rendimiento: hay muchos timeouts de espera de fences con la GPU poco ocupada. Es lo siguiente que se investiga.
- Triángulos sueltos en la animación al abrir ventanas. El traductor da lo mismo que Metal en el host, así que el fallo está en el dispositivo.
- Mosaico al girar la tablet.
- Cuelgues ocasionales en el reinicio en caliente (el arranque en frío va bien).
- No hay batería en macOS (el driver SPMI ADC de iPadOS está pendiente de portar), ni gobernador de CPU por temperatura.
- No hay escalado HiDPI; se usa `native-gop-scale=2`.

## Contenido

| Carpeta | Qué hay |
| --- | --- |
| `scripts/` | Ciclo de pruebas (`macos-cycle.sh`: pull del log, flasheo de boot_a + vendor_boot, guardas del log), instalación del bundle de la GPU (`install-gpubundle.sh`), construcción del kernel, la imagen `vendor_boot` (`make-macos-image.sh`), el contenedor APFS completo (`fullos-volumes.sh`, `fullos-populate.sh`, `apfs-set-volume-group.py`), la configuración sin asistente (`macos-skip-setup.py`) y el flasheo de userdata compuesto al vuelo (`macos-full-root.py`) |
| `docs/` | `MACOS-RETURN.md` (cómo montar macOS completo desde cero) y `METAL-MACOS-PLAN.md` (GPU y Metal) |
| `gpu/` | El puerto de la GPU a macOS: preparación de fuentes, parches, código glue del kernel (`src/macos`), el plugin Metal y Turnip, la ventana DRV y el primado de la caché de shaders en el host (`hostcache/`) |
| `touch/` | Driver de táctil para macOS (GENI SPI y Novatek), sin firmware |
| `patches/forsheng/` | Cambios sobre macOS-ForSheng (base `ca57db7`, rama 26.5): los commits (`commits/`), los cambios sin commit (`worktree.diff`) y los ficheros nuevos (`new-files/`) |
| `patches/unixnu/` | Cambios sobre el UniXNU que usa ForSheng y sus submódulos (ArmCpu, DarwinBoot, NativeUfs…); las bases están en `BASE.txt` |
| `patches/uefi/` | Cambios sobre el UEFI UniXNU de la tablet (`BASE.txt`, `uefi-uke-fork.diff`) |
| `patches/device/` | Cambio en `make-uke-adt.py` del repo de dispositivo (`--os-environment` y similares) |

Las rutas de los scripts son las de la máquina de desarrollo (`/Users/daniel/Desktop/Deliver/Domotica`): hay que adaptarlas.

## Lo que NO está aquí

No se publica, y cada uno tiene que conseguirlo por su cuenta:

- el firmware de la GPU (gen71100: GMU, SQE y ZAP) y el del táctil (Tianma);
- las imágenes de Apple (IPSW, caché dyld, cryptex, KDK) y las cabeceras privadas de IOHIDFamily;
- el identificador único del chip (`chip-id.txt`), las claves y las imágenes generadas;
- las fuentes que `gpu/stage.py` copia de UniXNU-a27 y UniXNU-uke (se regeneran con ese script).

## Lecciones importantes

- **Dock y barra de menús:** si se salta el asistente, hay que escribir también las claves de versión de `com.apple.SetupAssistant` (`LastSeenBuddyBuildVersion`, `PreviousBuildVersion`…) y `OptimizerPreviousBuild` en `loginwindow`. Sin ellas, loginwindow cree que es una actualización y espera a MiniBuddy. Ver `scripts/macos-skip-setup.py`.
- **Cryptex sin ignition:** la caché dyld tiene que estar en `/System/Library/dyld` y las librerías sueltas del cryptex en sus rutas reales del volumen del sistema.
- **Metal y SIP:** Metal solo carga plugins de confianza para SIP. Con `lp-sip0=0` ignora el plugin; hace falta `SIP0=0x7f`.
- **Táctil:** compilar siempre con `UKE_TOUCH_SRC` exportado y comprobar `UkeTouchPointer` en el prelink.

La cuenta de prueba de la tablet es `uke` con contraseña `uke` (solo local).

Los parches sobre ForSheng y UniXNU mantienen la licencia de esos proyectos.
