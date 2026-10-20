# UFC-71: límites de transferencias de imagen

Ported from PORTHEON (XAndromedaXR/UFC-4-PS4-PORT) d1108a7 aadb446 ef8e40a

Adaptación sobre ufc3-online. Los chequeos están siempre activos; no se porta el escape
`BB_COPY_CHECK=0`, que permitiría enviar accesos inválidos al driver.

- `vk_runtime.cpp`: uploads y downloads filtran cada región inválida; las válidas continúan.
  También se chequean ambas patas de las copias mediante buffer intermedio y ambos extremos
  de las copias imagen→imagen (completas, mip 0, mip/slice, depth/stencil y resolves).
- `vk_copy_bounds.h`: validación compartida, sin dispositivo Vulkan, de mip, capa, offset,
  extensión y bytes. Usa las dimensiones/formato del backing Vulkan; calcula en 64 bits,
  detecta overflow, redondea bloques BC hacia arriba y contempla copias 2D array↔3D.
- `tile_manager.cpp/.h`: límites de ambos buffers, offsets, mip layouts y cantidad de
  parámetros antes de construir el pase. Se descarta un pase cuyo layout exceda el rango
  del descriptor o cuya lista de mips no cubra los bytes que recorrería el shader.
- `vk_breadcrumbs.cpp`: los pases muestran la dirección guest de la imagen, dimensiones,
  niveles y bytes. Los bytes de un pase no se interpretan como un slot de dispatch indirecto.

Los rechazos descartan regiones/pases completos; no se achican silenciosamente extensiones.
Ejemplos de log (valores ilustrativos):

```text
Copy check: upload of image 0x12340000 skipped: level 0 layers 0+1 extent 16x8x1 at 0,0,0, buffer offset 0 size 512 of 511
Copy check: image copy 0x12340000 -> 0x56780000 levels 0 -> 0 layers 0+1 -> 0+1 extent 16x8x1 dropped
Copy check: tiling to memory of image 0x12340000 ... skipped: tiled ... linear ...
```

Cada categoría de rechazo imprime hasta 20 líneas por proceso (contadores saturados).
Se conserva el diagnóstico `Image copy: ...` para las primeras 300 copias y
`Tiling to memory: ...` una vez por dirección guest, hasta 200 imágenes.
Un breadcrumb de transferencia/tiling incluye `(image 0x12340000 16x8, levels 1, 512 bytes)`.

## Validación

`ufc3-build --gpu-tests` compila el port y los doce targets de tests de `gpu/CMakeLists.txt`,
los ejecuta de a uno bajo el mismo lock y valida el SPIR-V de motion con `spirv-val` si está
instalado. Se pueden seleccionar targets con `ufc3-build --gpu-tests nombre...`.
El runner continúa después de un fallo y devuelve estado no cero si alguno falla.
Se completaron los stubs compartidos de runtime/diagnóstico del host que faltaban para
enlazar los tests existentes, sin modificar el runtime de producción.
`copy-bounds-test` prueba bordes exactos, offsets negativos, overflow,
mips/capas inválidos, strides, mips BC pequeños, depth/stencil y volumen↔array, además
de layouts de tiling incompletos o demasiado grandes. No abre Vulkan ni ejecuta el juego.
Los tests existentes de scene resolution, TAA y camera motion sí usan Vulkan.

La comprobación manual en Create Fighter queda a cargo del usuario: revisar `Copy check:`
y, si ocurre device lost, el nombre/dirección de la textura del breadcrumb.

## Resultado en este worktree

- `ufc3-build`: OK (PGO off, LTO ON). También compilan los doce targets de tests.
- Pasan 7: copy-bounds, motion-history, motion-shader, ui-composition, label-guard,
  write-window-merge y page-watch-perms. Pasa `spirv-val` sobre los cuatro shaders motion.
- Fallan 2 tests existentes: `settings-test` (línea 31) y `upscaler-support-test` (línea 27).
  Esperan restaurar FSR3/TAA desde la configuración, pero `BbSettings::Load()` fuerza
  `UpscalerOff` desde antes de este ticket. Su política y sus expectativas quedan sin tocar.
- No pueden ejecutarse 3: scene-resolution, taa-shader y camera-motion. Vulkan devuelve
  `ErrorInitializationFailed` al enumerar dispositivos físicos, antes de ejercer las copias.
  Sólo está instalado el ICD de NVIDIA; no hay Lavapipe disponible en este entorno.
- Logs: `out/UFC-71-build.log`, `out/UFC-71-gpu-tests.log` y
  `out/UFC-71-gpu-tests-remaining.log`.

Pendiente para cumplir el criterio de suite verde: resolver por separado la discrepancia
preexistente de upscalers y volver a correr los tres tests de Vulkan en un entorno con
un dispositivo disponible (`ufc3-build --gpu-tests scene-resolution taa-shader camera-motion`).
No se ejecutó el juego ni se creó un commit. No se modificaron `buffer_cache.cpp` ni
`page_manager.*`. Los parches que la compilación aplica al submódulo FSR se revirtieron al
terminar para dejar únicamente los cambios del ticket.
