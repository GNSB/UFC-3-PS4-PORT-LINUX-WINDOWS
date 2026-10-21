# UFC-90: caché del driver y warmup opt-in

Se adapta el enfoque GPL de `FelipePrado0_SpartanPort` (vendor shadPS4 de la
misma familia): cargar etapas en orden, construir pipelines en workers y publicar
los resultados desde el hilo GPU. Se reutiliza nuestro `PipelineCompiler`;
no se incorporan las implementaciones sin licencia de iss_pc/tlg_pc.

## Variables

| Variable | Predeterminado | Efecto |
| --- | --- | --- |
| `BB_DRIVER_PIPELINE_CACHE=1` | apagado | Carga y guarda el blob Vulkan del driver. Independiente del almacén de shaders. |
| `BB_BACKGROUND_WARMUP=1` | apagado | Encola la compilación de pipelines conocidos y continúa el arranque; un worker si no se activa paralelo. |
| `BB_PARALLEL_WARMUP=1` | apagado | Usa entre 1 y 4 workers (núcleos/4). Sin background, espera al warmup al arrancar. |
| `BB_FRAME_STATS=1` | existente | Agrega `N hot pipelines` a cada ventana de estadísticas de cinco segundos. Cuenta graphics/compute compilados a demanda, incluidos los graphics asíncronos existentes; excluye warmup. |

Se mantienen `BB_PIPELINE_CACHE`, `BB_ASYNC_PIPELINES`,
`BB_ASYNC_PIPELINE_THREADS` y `BB_PREP_WORKERS` con su comportamiento previo.
Las variables nuevas se leen en `gpu/shim/core/emulator_settings.h`, donde ya
se definen las opciones del video core; no se escriben en bbport.ini ni en el launcher.
Las claves y versiones del almacén de shaders no cambian.

## Archivo nuevo

`<GPU user dir>/cache/driver_pipeline_cache/<vendor>-<device>-<driverVersion>-<pipelineCacheUUID>.bin`

Los IDs y la versión cruda del driver están en hexadecimal. La ruta completa
aparece al arrancar y al guardar. El directorio GPU se selecciona con
`BB_GPU_USER_DIR`; `run.sh` lo configura desde `BB_USER_DIR` o `<BB_DATA_DIR>/user`.
Si no existe el directorio indicado, se aplica el fallback de `Common::FS`
(`~/.local/share/shadPS4` en Linux sin XDG, `%APPDATA%/shadPS4` en Windows).

El archivo contiene un sobre de 16 bytes (magic/version propia y checksum FNV-1a
64), seguido del blob Vulkan. Se comprueban longitud (máximo 256 MiB), checksum,
header Vulkan v1, vendorID, deviceID y UUID antes de pasarlo al driver.
Un archivo inválido o rechazado se descarta y elimina; se crea una caché vacía.
Un cambio de driver selecciona otro nombre de archivo.
Los archivos de drivers anteriores quedan en disco y se pueden eliminar con el juego cerrado.

El guardado escribe `.bin.tmp`, cierra el archivo y reemplaza el destino con
rename en POSIX o `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` en Windows.
Se conserva el blob anterior si falla la escritura/reemplazo.
Se guarda al cerrar normalmente la ventana (X, Alt+F4 o Salir), antes de que
el host termine sin destructores, y también desde Sync/destructor si se usan.
No se garantiza guardar compilaciones nuevas tras un crash o terminación forzada.

## Concurrencia y alcance

La lectura de metadatos, validación de permutaciones y creación de shader modules
siguen siendo síncronas. Sólo la construcción de pipelines conocidos (graphics
y compute) se deriva a workers. Se copian los Info usados al construir para que
la actualización de user data del hilo GPU no compita con los workers; al publicar
se restauran las referencias a los programas vivos. Los mapas sólo se modifican
desde el hilo GPU y se publican hasta ocho resultados por selección.
Si un pipeline hace falta antes de completar su warmup se compila por la ruta
normal, sin omitir draws; el resultado duplicado de warmup se descarta.

Se crea el VkPipelineCache **antes** del warmup (el orden anterior lo usaba antes
de crearlo). Vulkan conserva la sincronización interna del cache: no se activa
`EXTERNALLY_SYNCHRONIZED`. El guardado se serializa para no competir por el tmp.
También se restaura el RuntimeInfo de cada etapa al precargar y se evita dejar
programas nulos si el driver rechaza un shader module.

No se implementa `VK_KHR_pipeline_binary`. La compilación de compute nuevo durante
una pelea sigue siendo síncrona; sólo el compute conocido entra en warmup.

## Prueba manual (una ejecución a la vez)

1. Con el juego cerrado, respaldar/mover las cachés existentes del juego dentro de
   `<GPU user dir>/cache` (la carpeta de serial del juego y `driver_pipeline_cache`).
   No borrar saves ni el directorio user completo.
2. Primera pelea, caché vacía, por el comando habitual:

   ```sh
   BB_DRIVER_PIPELINE_CACHE=1 BB_BACKGROUND_WARMUP=0 BB_PARALLEL_WARMUP=0 BB_FRAME_STATS=1 ./run.sh
   ```

   Repetir una escena/efectos reconocibles. Cerrar por X/Salir y comprobar
   `Driver pipeline cache: saved N bytes to ...`.
3. Repetir la misma pelea, configuración y efectos:

   ```sh
   BB_DRIVER_PIPELINE_CACHE=1 BB_BACKGROUND_WARMUP=1 BB_PARALLEL_WARMUP=0 BB_FRAME_STATS=1 ./run.sh
   ```

   Comprobar `loaded N bytes`, el número de workers y `Warmup completed`.
   Comparar `worst frame`, `hot pipelines`, tiempo de compilación y tirones visibles.
   El arranque puede llegar al primer draw antes de que termine el warmup;
   los pipelines que aún falten se compilan normalmente.
4. Opcionalmente repetir agregando `BB_PARALLEL_WARMUP=1` para comparar CPU/tiempos.
   Para aislar el blob del driver, comparar también con warmup apagado.
5. Recuperación: con el juego cerrado, respaldar el `.bin` y truncarlo o alterar
   un byte. El siguiente arranque debe informar el motivo del descarte y continuar;
   al cerrar debe producir un archivo válido nuevo. Repetir dos cierres en Windows
   verifica también el reemplazo de un destino ya existente.
6. Para volver al comportamiento predeterminado, quitar las tres variables nuevas
   o ponerlas en `0`. No hace falta cambiar configuraciones del launcher.

Test corto sin juego: `ufc3-build --gpu-tests driver-cache-blob`.
Compilación Windows: `ufc3-build --windows-runtime`.
La comprobación visual de rendimiento y la ejecución en drivers reales corresponden
al usuario; el agente no ejecuta el juego.

## Validación del agente

- Linux: `ufc3-build --gpu-tests driver-cache-blob`, LTO ON, compilación y test aprobados.
- Windows x86-64: `ufc3-build --windows-runtime`, LTO ON, aprobado; binarios PE
  y marcador de versión verificados por el script. Un primer intento tuvo un
  SIGSEGV interno del lexer de Clang; el reintento completo pendiente pasó.
- `git diff --check`: aprobado. No se ejecutó el juego ni se verificó rendimiento real.
- El build aplica sus parches habituales a `gpu/third_party/fsr-vulkan`; no hay
  cambios de feature en ese submódulo.

Archivos de implementación: `vk_pipeline_cache.{h,cpp}` y
`vk_pipeline_serialization.cpp` (persistencia, cola/publicación y warmup),
`vk_pipeline_common.h` (acceso de PipelineCache para restaurar referencias),
`vk_driver_cache_blob.h` (validador), `gpu/shim/core/emulator_settings.h` (env),
`gpu/shim/bbgpu.cpp` (checkpoint al cerrar),
`gpu/shadps4/core/libraries/videoout/driver.cpp` (contador),
`tests/test_driver_cache_blob.cpp` y `gpu/CMakeLists.txt` (test corto).
