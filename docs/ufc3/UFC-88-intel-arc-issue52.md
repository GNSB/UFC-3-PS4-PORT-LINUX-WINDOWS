# UFC-88 — Intel Arc A580, escena 3D negra (#52)

## Resultado y alcance

Port adaptado de PORTHEON `d7b35aa7dcc45180bda71a2625a4ad89ef52e06f`, con los
arreglos Intel de mancelli (`8034718c`, tag `ufc4-intel-arc-2026-10-14`). El antecedente
es UFC 4 probado en una B580; **todavía no hay validación visual de UFC 3 en A580**.
No se ejecutó el juego. No se hicieron commits ni operaciones sobre el índice.

Texto que debe incluir el commit del despachador:

> Ported from PORTHEON (XAndromedaXR/UFC-4-PS4-PORT) d7b35aa; Intel fixes by mancelli (shadPS4 fork, ufc4-intel-arc-2026-10-14)

## Evidencia del reporte

Se leyó el log completo de `~/ufcCOntext/issue52/20261020_175215.log`, el issue
[52](https://github.com/GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS/issues/52), su comentario
con el log y la captura adjunta. El formulario dice AppImage, pero el log corresponde
inequívocamente al launcher Windows (`sacredfire-game.exe`, rutas de Windows).
La captura muestra HUD correcto y escena casi negra; aún se distingue algo del alambrado.

- Línea 37: `Intel(R) Arc(TM) A580 Graphics`, 8098 MiB, `Intel Corporation
  101.9150 (101.9150)`, Vulkan `1.4.359`.
- **El log no imprime vendor ID, driverID Vulkan ni tamaños/rango de subgroup**.
  `0x8086` es el vendor esperado de Intel, pero no se puede presentarlo como dato
  confirmado por este archivo. Tampoco se puede afirmar 8/16/32 o qué stages permiten
  forzar 32 a partir de este log. Se añadieron diagnósticos stdout para la próxima prueba.
- No aparecen errores de compilación/creación de shaders o pipelines, device lost ni
  crash. El cierre es normal, código 0. El único `<Error>` es el stub
  `sceVideoOutSetWindowModeMargins` (línea 193), sin evidencia de relación con el fallo.
- El juego sigue enviando cientos de draws por frame; al final ronda 85–93 FPS. La
  ausencia de compilaciones en varios intervalos de estadísticas no demuestra que
  los shaders estén correctos (pueden venir de la cache).

Extensiones anunciadas como ausentes (líneas 38–49): `VK_KHR_maintenance8`,
`VK_AMD_buffer_marker`, `VK_EXT_attachment_feedback_loop_dynamic_state`,
`VK_EXT_depth_range_unrestricted`, `VK_AMD_shader_explicit_vertex_parameter`,
`VK_KHR_fragment_shader_barycentric`, `VK_AMD_shader_image_load_store_lod`,
`VK_AMD_gcn_shader`, `VK_AMD_shader_trinary_minmax`, `VK_NV_framebuffer_mixed_samples`,
`VK_AMD_mixed_attachment_samples`, `VK_KHR_external_memory_fd`. Es el inventario de
capacidades opcionales; el log no atribuye un pipeline fallido a ninguna de ellas.
Para breadcrumbs ya se indica el fallback a timestamp queries.

Formatos anunciados como incompletos (líneas 50–57):

| Formato | Capacidades ausentes que informa el log | Lectura del hallazgo |
| --- | --- | --- |
| R8Srgb / R8G8Srgb | SampledImage, ColorAttachment, TransferSrc/Dst | Hay fallback existente a R8Unorm/R8G8Unorm en `GetSupportedFormat`. |
| B10G11R11UfloatPack32 | **Sólo VertexBuffer** | No indica que falte ColorAttachment o SampledImage para el color de escena. |
| A2B10G10R10SnormPack32 / SintPack32 | SampledImage, StorageImage, ColorAttachment, TransferSrc/Dst, storage sin formato | No prueba que la escena use estos formatos; inventario global de formatos. |
| R5G5B5A1UnormPack16 / B4G4R4A4UnormPack16 | StorageImage, storage sin formato | No hay error de uso en este log. |
| D16UnormS8Uint | DepthStencilAttachment | Hay fallback existente a D32SfloatS8Uint (luego D24 si hace falta); merece conservarse como alternativa de diagnóstico. |

En la línea 357 se ve una vista depth-stencil como D32SfloatS8Uint usando sólo el
aspecto depth. En 723–728 se registran las seis conversiones de la sombra a R16_UINT,
una por capa, y después hay conversiones y reutilizaciones sostenidas. Los mensajes
`Image copy ... D16UnormS8Uint` describen el formato lógico de la imagen; no demuestran
que se haya creado un VkImage con un formato no soportado. El código existente de
`DepthAsColor` convierte depth flotante a bits Z16 (`round(depth * 65535)`) antes de
los lectores R16_UINT. No hay evidencia suficiente para atribuir el negro a esa
conversión ni para descartarla. No se añadieron cambios de formatos/depth especulativos.

**Hipótesis de trabajo:** sincronización LDS/subgroups y propagación de NaN en la
cadena de color son compatibles con el síntoma y tienen un arreglo upstream concreto.
El log por sí solo no confirma la causa; el resultado visual de la única prueba pedida
al usuario decidirá si este port resuelve #52.

## Implementación

- `shader_recompiler/backend/spirv/emit_spirv.cpp`: workgroups compute declarados
  1D sólo con el perfil Intel. El loop guard de UFC-68 queda intacto.
- `emit_spirv_context_get_set.cpp`, `spirv_emit_context.cpp`: reconstrucción de los
  IDs locales X/Y/Z a partir del índice plano; no se declara dos veces ese input.
  NaN a cero en exports de color flotante, conservando exports enteros.
- `emit_spirv_image.cpp`: NaN a cero por componente en escrituras float de storage
  images; sin modificar escrituras enteras.
- `emit_spirv_barriers.cpp`, `ir/passes/shared_memory_barrier_pass.cpp`: barreras LDS
  para grupos de 64 y sincronización de subgrupo entre lecturas/escrituras LDS en
  bloques divergentes, tal como en d7b35aa.
- `profile.h`: los tres nuevos campos de perfil quedan apagados al inicializar.
- `vk_instance.h`, `vk_compute_pipeline.cpp`, `vk_graphics_pipeline.cpp`: subgroup
  requerido por stage. Se conserva el 64 previo donde estaba disponible. En Intel,
  se pide 32 cuando hay size control, default/max 32, mínimo menor de 32 y el stage
  está en `requiredSubgroupSizeStages`; 0 deja elegir al driver. Los structs Vulkan
  por stage mantienen su vida hasta la creación del pipeline.
- `vk_pipeline_cache.cpp`: decide los flags, fija la variante **antes de WarmUp** y
  registra cada arreglo y el tamaño pedido en cada stage.
- `vk_instance.cpp`: imprime vendor/device/driver y tamaños/rango/stages de subgroup
  para Intel, incluso si se apagan los arreglos.
- `vk_pipeline_serialization.*`: conserva versión binaria **12**, metadata 10 y key 7.
  Variante Intel en bits **16–20** y loop guard apagado en bit **31**, sin colisión.

Diferencias intencionales respecto de PORTHEON:

1. Todos los overrides se limitan a `eIntelProprietaryWindows` y
   `eIntelOpenSourceMESA` (Mesa ANV). No se pueden activar estos cambios en NVIDIA o
   AMD ni con variables de entorno: es una restricción de UFC-88.
2. `BB_INTEL_FIXES=0` también apaga el nuevo forzado a 32; en PORTHEON este era
   independiente. Se mantiene la precedencia del switch individual sobre el global.
3. También se distingue en cache el forzado a subgroup 32, para no reutilizar los
   binarios de la configuración opuesta en Intel.
4. La variante ocupa sólo cinco bits; nunca puede pisar el bit 31 de UFC-68.

En NVIDIA/AMD, los nuevos flags permanecen en falso y la selección de subgroup de
64/driver conserva el comportamiento previo; no cambian shaders ni versiones de cache.

## Activación y diagnóstico

Se activan por defecto únicamente con el driver Intel Windows o Mesa ANV:

| Variable | Efecto con `0` / `1` |
| --- | --- |
| `BB_INTEL_FIXES` | Apaga / enciende los defaults de los cinco arreglos. |
| `BB_SUBGROUP32` | Desactiva / habilita la petición de subgroup 32 en stages compatibles. |
| `BB_NAN_GUARD` | Desactiva / activa NaN a cero en color float y storage image float. |
| `BB_FLATTEN_WG` | Desactiva / activa workgroups 1D e ID local 3D reconstruido. |
| `BB_LDS_DIVERGENT` | Desactiva / activa sincronización LDS dentro de bloques divergentes. |
| `BB_INTEL_LDS_BARRIERS` | Desactiva / activa las barreras LDS para grupos de 64. |

Las variables individuales prevalecen sobre `BB_INTEL_FIXES`, como en PORTHEON.
Para apagar todo, usar `BB_INTEL_FIXES=0` sin overrides individuales. Los nombres
conservan el prefijo `BB_` para compatibilidad con el parche fuente. Reiniciar el
proceso tras un cambio; no se expone otra opción en el launcher.

Buscar estas dos líneas al iniciar:

```text
GPU: Intel device vendor=0x8086 device=... driver=IntelProprietaryWindows 101.9150; subgroup default/min/max=..., size control ..., required stages ...
GPU: Intel workarounds: NaN guard on, 1D compute workgroups on, LDS barriers on/on; required subgroup VS/TCS/TES/GS/FS/CS=... (0=driver choice), shader variant 0x...; ...
```

El primer mensaje permite confirmar los datos que faltan en el reporte actual.
El segundo muestra los flags efectivos (LDS normal/divergente), no sólo las
variables solicitadas. La variante esperada con los cuatro flags y subgroup 32
aplicables es `0x1f`; si el hardware no necesita/permite forzar 32, es `0x0f`.
No prometer que todos los stages pedirán 32: depende de las capacidades declaradas.

Versión de cache: `12 | (variant << 16) | (loop_guard_off ? 0x80000000 : 0)`.
Por ejemplo, sin Intel sigue `0x0000000c` / `0x8000000c`; con variante `0x1f` es
`0x001f000c` / `0x801f000c`. No hace falta borrar la cache manualmente: el loader
rechaza metadata con versión binaria distinta y recompila. Como en PORTHEON, la
variante invalida/reemplaza entradas de la misma cache; no crea otro directorio
ni conserva simultáneamente todas las configuraciones.

## PORTHEON “Intel Arc mode”

Se inspeccionaron las ramas `portheon/*` y las cabeceras del bundle
`~/ufcCOntext/portheon/portheon-source.bundle`. Su punta UFC 4 es `efab7e7`,
**UFC 4 0.2.6 / PORTHEON 1.6** (18 de octubre). No incluye la versión/commit 2.7
mencionada en el ticket ni un modo Intel Arc de launcher. No se hizo fetch ni
se modificaron refs.

UFC 3 ya tiene `BB_NEO=0` por defecto (`neo_mode()` en `runtime_kernel.c`).
El log usa Immediate. VSync limita el ritmo de presentación, pero este log no
demuestra que sea condición para corregir shaders o depth. Se conserva el default
actual; no se copió un preset de launcher no disponible. Si después se proporciona
el commit 2.7, queda pendiente revisar su justificación concreta.

## Verificación

- `ufc3-build --test`: **PASS**, código 0. Se repitió al final para cubrir los
  últimos cambios de diagnóstico/selección de variante hechos durante la primera
  compilación. Ambas ejecuciones pasaron. Incluye compilación de GPU/runtime Linux,
  identidad de versión, write-window, pad, runtime, audio, file mods, semáforos,
  FP determinista, AppContent, trofeos, event flags, red/tiempo y los **42 tests**
  Python del servidor. No se inició el juego ni se creó una ventana del juego.
- `LLVM_MINGW_ROOT=/home/myzk/Projects/ufc3-native/out/windows/toolchain/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64 ufc3-build --windows-runtime`:
  **PASS**, código 0, LTO ON. La opción está soportada por el wrapper; `--test`
  por sí solo no hace cross. Se reutilizaron las dependencias existentes de sólo
  lectura (symlink local `out/windows/sysroot`); build y binarios dentro del worktree.
  Generó `out/windows/bin/{sacredfire-game.exe,SacredFire.exe,bb-gpu-capabilities.exe}`,
  verificados como PE x86-64 y con marcadores de identidad de versión correctos.
  No se ejecutaron binarios Windows.
- Identidad de ambas builds: `ufc3-v0.1.12-dirty`. No es todavía una release ni
  un paquete distribuible nuevo; el despachador debe generar/publicar la build de prueba.
- `git diff --check`: **PASS**. La revisión del diff conserva el loop guard, y los
  archivos del emisor/barreras importados coinciden con d7b35aa salvo la adaptación
  del entry point sobre UFC-68. No hubo errores de compilación; CMake emite avisos
  de política y GCC/LTO avisos de enums de FSR, fuera de este cambio.
- Logs guardados en `out/UFC-88-linux-test.log`, `out/UFC-88-windows-build.log`,
  `out/gpu-build.log` y `out/windows/build.log` (ignorados por Git).
- El build aplica automáticamente los parches existentes a `gpu/third_party/fsr-vulkan`,
  por lo que ese submódulo aparece con working tree sucio. Su commit/puntero no cambia;
  no hay modificaciones manuales de FSR en este ticket.
- No se midió rendimiento ni se verificó rendering en Intel, NVIDIA o AMD. Los flags
  apagados y la versión de cache original se preservan para NVIDIA/AMD por código;
  la validación visual de la A580 queda para la única prueba manual descrita abajo.

## Una sola prueba manual y borrador de respuesta para #52

Publicar el enlace de la build que prepare el despachador; no hay build publicada
por este ticket. Pedir una ejecución con la build nueva, mismos ajustes y driver,
sin overrides de estos arreglos: entrar a Quick Fight con Nurmagomedov/McGregor,
ver si vuelven los peleadores/octágono/fondo, cerrar y adjuntar log y captura de
esa misma ejecución. No pedir A/B, borrado de cache ni actualización de driver
como parte de esta primera prueba.

Borrador en inglés (no publicado):

> We've prepared an Intel-specific test build with shader fixes ported from PORTHEON and mancelli's shadPS4 fork: **[build link]**. Could you do one Quick Fight run with Nurmagomedov vs. McGregor on your A580, keeping your current driver and settings, and tell us whether the fighters, octagon and background now render? Please attach the launch log and a screenshot from that same run. The fixes enable automatically; no cache deletion or special options are needed. This is not yet visually verified on an A580.
