# UFC-69: copias de libc junto a datos GPU

Ported from PORTHEON (XAndromedaXR/UFC-4-PS4-PORT) 074f3cd

Origen: `074f3cd0ed2c9a1765fa576c12e808096fabda25`, basado en nuestra 0.1.5
(`665b090`), GPL-2.0-or-later. El despachador debe incluir la línea anterior
literalmente en el mensaje del commit. Este worktree se entrega sin commit.

## Implementación

- `src/runtime.c`, `src/runtime.h`, `src/runtime_memory.c`: las tres primitivas
  guest `memcpy`, `memmove` y `memset` usan el único rango thread-local
  existente, `copy_begin/copy_end`, con sus signal fences. Tanto
  `FaultInHostCopy` como `NoteFaultingHostCopy` consultan
  `runtime_guest_copy_range()`, que también usa `DecideWriteStep`.
  Después de `copy_range_clear()`, una única llamada a
  `runtime_memory_host_copy_done(dst, size)` notifica las ventanas abiertas.
  `runtime_memory.c` sólo conserva el hook de finalización, el contador de
  ventanas abiertas y el interruptor `BB_HOST_COPY_EXTENTS=0`; no duplica el
  rango. `runtime_memory_host_copy_enabled()` consulta si el hook está puesto:
  sin hook, `FaultInHostCopy` devuelve false aunque haya una copia en curso.
- `gpu/shadps4/video_core/page_manager.{cpp,h}`: cada `WriteWindow::known`
  recuerda los granules completos de 8 bytes escritos por una copia. Al cerrar,
  esos bytes pasan a la CPU aunque su valor no haya cambiado; el diff procesa el
  resto. Sólo las apropiaciones por diff pueden bloquear un sitio. Un readback
  sobre una ventana abierta conserva también sus bytes conocidos.
- `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp`: una copia host con
  extent puede abrir una ventana incluso si su sitio estaba bloqueado. Se
  conservan los chequeos de proximidad/solapamiento, el rango de la copia
  redondeado a 16 bytes y `CoverStore` de UFC-61. Por lo tanto, una copia que
  realmente cubre datos GPU sigue necesitando el camino de coherencia existente.
  La optimización se aplica a copias junto a esos datos. No se cambia ningún
  valor por defecto de write windows, preupload ni write step (UFC-49).
- `gpu/shadps4/video_core/write_window_merge.h`: variante de merge que permite
  conservar los granules conocidos sin exigir un cambio de valor.
- Tests: `tests/test_write_window_merge.cpp`, `tests/test_runtime.c` y
  `tests/test_gpu_runtime_stubs.cpp`. Cubren reescrituras del mismo valor durante
  un readback, payloads parciales, callbacks de las tres primitivas después
  de limpiar el rango, hook desactivado, ausencia de ventanas abiertas y rangos
  vacíos o con overflow; conservan las regresiones UFC-48/UFC-61.

Los modos `BB_WRITE_WINDOW=1` y `2` usan el mismo ownership nuevo. El modo 2
sigue requiriendo rango exacto; no se relaja su comprobación de los bytes vecinos
que un cierre devolvería a la GPU. Sólo se marcan como conocidos granules
completamente incluidos en la copia: los extremos parciales siguen por diff.

La telemetría de ventanas, cada cinco segundos junto a las estadísticas
existentes, incluye esta línea sólo si `copy_windows || copy_bytes`:

```text
Write windows: N/s opened by libc copies, K KiB/s of their bytes known (no readback)
```

`N` cuenta aperturas nuevas por libc, no reprotecciones. Los bytes se cuentan una
vez por ventana, aunque la copia sea notificada al fault y de nuevo al terminar.

## Issue #47: evidencia y alcance de la mejora

Se consultó con `gh issue view 47 -R GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS` usando
el binario instalado (el wrapper de mise intentaba actualizarse en una ruta
sólo lectura). Fuente: [issue #47](https://github.com/GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS/issues/47),
[Logs.zip adjunto](https://github.com/user-attachments/files/33704900/Logs.zip).
El autor identifica Steam Deck, Linux, v0.1.9.

ZIP SHA-256: `73c965c2d3e1f0c966ff61832a5792bb9fc5f6b640734c715e415b4247e71d88`.
Se analizó una sola copia del log (`20261020_090243.log`), evitando duplicar
`launcher-run-1792476163045686647.log`, más sus CSV de frames/readbacks.
Son 155.319 frames, hasta `t_s=4830.9608` (80,5 minutos), y 963 informes de FPS.

| Intervalo CSV | FPS (frames/300 s) | Handler de escritura, ms/frame promedio | Read faults totales |
| --- | ---: | ---: | ---: |
| 300–600 s | 35,96 | 8,71 | 6 |
| 900–1200 s | 27,36 | 16,35 | 0 |
| 4500–4800 s | 29,03 | 14,41 | 19 |

La caída existe, pero **no coincide con una acumulación de sitios libc bloqueados
ni con readbacks crecientes** en esta captura:

- Sólo hay dos altas de sitios bloqueados: guest `+0x53175b` y el allocator
  `+0x1262c9f`. Aparecen aproximadamente a los minutos 71 y 80, respectivamente
  (estimados por los informes de cinco segundos), después de la caída inicial.
- Entre 948 líneas que informan faults de sitios bloqueados, sólo una es distinta
  de cero: `1.0 faults/s`, al final. No se llena la tabla de sitios bloqueados.
- Los 27 informes `Readback sites (write faults, this window)` suman 334 faults
  que tomaron ese camino. El máximo aislado es 284 en un intervalo (~56,8/s),
  asociado a `+0x53175b` cerca del minuto 71. Las únicas dos entradas host libc
  informadas son de un fault cada una y páginas con `0 GPU B`.
- El CSV de readbacks contiene 444 registros: 315 de escritura y 129 de lectura.
  No es un contador de todos los faults y no debe confundirse con FPS ni con
  todos los downloads. Los read faults del CSV de frames también son escasos.

Sí crece el coste de los handlers de escritura en la fase lenta. El número de
draws y la escena cambian; no es una comparación controlada. Hay MAKO frame
generation activo, por lo que los FPS del port tampoco equivalen necesariamente
a los FPS presentados por el compositor. La caché crece algo durante la sesión,
pero esta evidencia no basta para identificar la causa de ese coste.

Conclusión: este port elimina el mecanismo problemático observado por PORTHEON,
pero el #47 **no demuestra que ese mecanismo cause su degradación**. No se debe
prometer que arregla esa captura. Hace falta el A/B siguiente para medir si ayuda
a ese usuario; investigar el coste de write faults restante queda fuera de UFC-69.

## Prueba manual para el usuario (no ejecutada por el agente)

Usar el mismo guardado de Career, mismos luchadores, arena y ajustes. Abrir el
proceso de cero para cada caso: los sitios bloqueados viven durante el proceso.
Repetir intro, pelea con golpes suficientes para ver sangre y sudor, retorno al
menú y una segunda pelea. Comparar iguales tramos; no mezclar menú e intro con
el combate al calcular FPS. Ejecutar los casos de a uno.

Desde el checkout, con los datos del juego configurados como de costumbre:

```bash
# A: comportamiento previo, modo 1.
env BB_HOST_COPY_EXTENTS=0 BB_WRITE_WINDOW=1 BB_FRAME_STATS=1 BB_SAVE_LOG=1 BB_LOG_DIR=logs/ufc69-m1-off bash run.sh
# Cerrar el proceso. B: extents activos (valor por defecto), modo 1.
env -u BB_HOST_COPY_EXTENTS BB_WRITE_WINDOW=1 BB_FRAME_STATS=1 BB_SAVE_LOG=1 BB_LOG_DIR=logs/ufc69-m1-on bash run.sh
# Cerrar el proceso. Repetir ambos casos con el modo exacto 2.
env BB_HOST_COPY_EXTENTS=0 BB_WRITE_WINDOW=2 BB_FRAME_STATS=1 BB_SAVE_LOG=1 BB_LOG_DIR=logs/ufc69-m2-off bash run.sh
env -u BB_HOST_COPY_EXTENTS BB_WRITE_WINDOW=2 BB_FRAME_STATS=1 BB_SAVE_LOG=1 BB_LOG_DIR=logs/ufc69-m2-on bash run.sh
```

Verificar la línea nueva de libc, altas de sitios bloqueados, `faults/s of barred
sites read back`, `Readback sites`, tiempo en write faults, FPS y peores frames.
Con extents apagados la línea nueva no debe aparecer, y debe aparecer el mensaje
de apagado. Con ellos activos debe registrar aperturas/bytes si se ejecuta ese
camino; tampoco se imprime si ambos contadores están en cero, para evitar
líneas periódicas sin actividad (observación de PORTHEON 3.2.1). Buscar menor coste y menos readbacks de libc en las mismas escenas.
Confirmar sangre/sudor, piel del luchador creado, ausencia de cuelgues y segunda
pelea correcta en ambos modos. Este ticket no aporta mediciones de FPS del juego.

## Validación en este worktree

Validación repetida tras la revisión del Jefe del 2026-10-20:

- `ufc3-build`: OK, salida 0.
- `ufc3-build --test`: OK, salida 0. Incluye `write window merge: ok`,
  tests de runtime con extents activos, y todos los demás checks del wrapper.
- `env -u BB_HOST_COPY_EXTENTS out/gpu/write-window-merge-test`: OK,
  `write window merge: ok`, salida 0.
- `env BB_HOST_COPY_EXTENTS=0 out/gpu/write-window-merge-test`: OK,
  `write window merge: ok`, salida 0. El test de merge es independiente de
  variables de entorno; ejercita las regresiones de ambos modos.
- `env BB_HOST_COPY_EXTENTS=0 out/runtime-test`: OK, salida 0. Verifica también
  que el hook esté desactivado y no se llamen callbacks en ese caso.
- `git diff --check`: OK.

La compilación emitió warnings de políticas CMake; no hubo errores. No se ejecutó el juego. El A/B de rendimiento y la verificación
visual de sangre/sudor, piel y segunda pelea quedan para el usuario, con los
cuatro comandos anteriores. No se cambió de rama ni se modificó el índice.
