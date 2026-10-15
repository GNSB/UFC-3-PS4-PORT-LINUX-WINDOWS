# UFC-13 / T4: captures de career y jugador ausente

## Resultado de la investigación (15 oct 2026)

No hay un fix confirmado. Se deja instrumentación opt-in `BB_T4_TRACE=1` para una
prueba manual. No se ejecutó el juego ni se cambiaron las políticas de readback.

Evidencia de `logs/20261015_195349.log` del checkout principal:

- Línea 2: `linear image readbacks 1`. `run.sh` ya activa `BB_READBACK_LINEAR=1`
  para UFC 3; el default genérico del shim sigue siendo false.
- `FindTexture` ya encola **todas** las storage lineales con dirección guest,
  incluyendo 2D >=64x64. El filtro de mancelli `e565b664` restringiría esa
  cobertura; no agrega un caso que falte aquí. También encolamos RT lineales.
- Las descargas se procesan antes de publicar fences EOP/EOS y en `OnSubmit`.
  `DownloadImageMemory` copia píxeles lineales: habilitar imágenes tiled allí
  sin retiling escribiría un layout incorrecto en memoria guest.
- Líneas 1111 y 1567: falla `open(/temp0/images/career)`; línea 1595:
  falla `open(/temp0/images/match)`. Ambas carpetas están ausentes en los datos
  inspeccionados. `runtime_file_configure` ya intenta crearlas desde `327737c`.

Los archivos `user/temp0/createplayer/cap_*/*.spg` tienen cabecera de 12 bytes,
seguida de un PNG grayscale de alfa y un JPEG de color. Se decodificaron ambos
con Pillow (no basta comparar tamaños de archivos):

| Capture | Dimensiones | Bytes | Contenido en cap_20002 |
| --- | --- | --- | --- |
| `win.spg`, `win_wfa.spg` | 600x1080 | 11683 | RGB negro y alfa cero en todos los píxeles |
| `left_medium.spg`, `right_medium.spg`, belt/wfa medium | 652x1080 | 12554 | RGB negro y alfa cero en todos los píxeles |
| `*_tott.spg`, `*_tott_belt.spg` | 1000x1200 | 20782 | Alfa cero; imagen invisible |
| `left_large.spg`, `right_large.spg` | 864x1360 | 286429 | Bloques/bandas de datos corruptos, no un retrato válido |
| `front.spg` | 360x260 | 136060 | Bloques de datos corruptos, no un retrato válido |

Los medium, win y tott también tienen alfa cero en `cap_10000` y `cap_20001`.
En esos dos directorios los large también son transparentes (20219 bytes).
Los captures se escribieron a las 19:54:37 (`cap_10000`, `cap_20001`) y
19:55:37–38 (`cap_20002`), antes del final de la sesión.

Esto confirma que el pipeline que produce/guarda retratos ya entrega imágenes
vacías o corruptas. No demuestra que el post-fight use `win.spg`, que falte un
draw del modelo, ni que tiled readback sea la causa. Los CSV no identifican el
archivo ni el formato/tiling de la imagen de origen: `.readbacks.csv` registra
faults/copias de buffers, no las descargas de `DownloadImageMemory` con su layout;
`.frames.csv` da draws/dispatches agregados, no por peleador. No hay un marcador
inequívoco de entrada al post-fight. Tampoco se puede atribuir la ausencia de las
carpetas a un fallo de creación: podrían haber sido eliminadas por el guest.

## Instrumentación

Con `BB_T4_TRACE=1`, todas las líneas nuevas llevan `[T4]`:

- `texture_cache.cpp`: bindings `render-target`, `storage`, `sampled` con tick,
  dirección, dimensiones, pitch, formato Vulkan, tile mode, flags, mips, capas y
  muestras. `linear_eligible` indica si cumple la política actual de descarga
  automática; un binding `sampled` por sí mismo no encola una descarga.
- `download-host`: staging completado, bytes no cero y hash XXH3 antes de la
  escritura guest; `guest-written` / `guest-write-failed`: resultado real de
  `TryWriteBacking`. `download-in-place` identifica la ruta sin copia CPU.
  No se agregan esperas GPU ni lecturas de memoria guest protegida.
- GPU: sólo imágenes 2D de color no BC >=64x64, hasta 512 combinaciones de
  evento/layout, 32 mensajes por combinación. El conteo de bytes incluye padding;
  un hash no cero no demuestra que el retrato sea válido. La ruta asíncrona de GC
  no tiene análisis de staging; las descargas encoladas usan la ruta síncrona.
- `runtime_file.c`: resultado de crear carpetas al configurar, opens con flags y
  errno negativo, primer read/write por descriptor con dirección del buffer,
  tamaño al cerrar, mkdir/rmdir/unlink/rename de captures y `temp0/images`.
  Esto permite saber qué `.spg` se genera y cuál se vuelve a abrir en cada menú.

## Una prueba manual pendiente

En una sola sesión, iniciar el build con `BB_T4_TRACE=1 BB_SAVE_LOG=1` (por ejemplo
antepuestos al comando habitual de `run.sh`, conservando el directorio del juego
y los datos habituales). Crear **un nuevo peleador de career**, guardar para
generar captures nuevos, abrir pause, ganar una pelea y quedarse unos segundos
en POST FIGHT MENU. Cerrar normalmente y entregar el `.log`, sus dos CSV y el
directorio `temp0/createplayer/cap_*` recién generado. No borrar saves ni reutilizar
solamente captures anteriores: una imagen ya guardada en blanco no se repara
con un cambio posterior de readback.

Buscar las dimensiones anteriores (o sus superficies intermedias/padded): si se
escriben en storage tiled sin conversión a lineal/descarga, investigar retiling
y coherencia CPU sobre esa dirección, en lugar de ampliar el memcpy lineal.
Si se completa `download-host` y `guest-written` pero el `.spg` sale vacío,
seguir el origen del render, sus canales y la conversión/codificación. Los opens
en post-fight permitirán comprobar si consume ese capture. Si el jugador falta
sin leer captures, hará falta inspeccionar los draws del modelo: este diagnóstico
no pretende resolver esa posibilidad con un cambio especulativo al texture cache.

## Validación del cambio

`ufc3-build` completó con código 0 para la versión final de la instrumentación;
se generó `out/sacredfire` (`ufc3-v0.1.2-50-g9ebeb1e-dirty`). `git diff --check`
sin errores. No se ejecutó el juego. La compilación aplica los parches existentes
del repo al submódulo fsr-vulkan; no se agregaron cambios propios allí. No se tocó
online, `vk_pipeline_cache.*` ni `shader_recompiler/**`. No se hizo commit.
