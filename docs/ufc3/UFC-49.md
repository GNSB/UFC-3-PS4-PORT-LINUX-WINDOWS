# UFC-49 — sangre y sudor ausentes desde 0.1.3

Investigación estática sobre `ufc3-online` (HEAD inicial `8de5098`). No se ejecutó el juego.
**Causa de la regresión aún no confirmada; no hay arreglo de render implementado.**
No se cambiaron defaults ni archivos de UFC-48 (`page_manager`/`buffer_cache`).

Validación: `ufc3-build` terminó con exit 0 y generó `out/sacredfire` y
`out/gpu/libbbgpu.so` en Linux. Hubo advertencias CMake de políticas y advertencias
LTO de tipos FSR preexistentes; ninguna detuvo el build. No se ejecutaron juego ni
pruebas largas. Se revirtió únicamente el parche automático de FSR que el build
aplicó al submódulo limpio, para dejar sólo este informe como cambio entregable.

## Cambios relevantes entre 0.1.2 y 0.1.3

Rango revisado completo: `git log --oneline ufc3-v0.1.2..ufc3-v0.1.3`.
Los merges y cambios de notas no introducen otro mecanismo independiente.

| Commits | Efecto y relación posible |
| --- | --- |
| `dcde0dd`, `4b644a8` | Apagan `BB_PREUPLOAD` y `BB_WRITE_STEP` en launcher Windows/run.sh para evitar piel negra/rota en creados/career. Cambia tanto la subida anticipada como la selección de fuente de texturas residentes. Principal hipótesis del ticket, todavía sin prueba con PREUPLOAD=1. |
| `f53d611`, `e4e9bb2`, `612684a` | Write windows: apagado en Windows, reactivado luego en Linux y Windows para 0.1.3. STEP=1/WINDOW=1 con PREUPLOAD=0 ya fue probado por el usuario sin recuperar sangre/sudor; no repetir esa combinación como diagnóstico. |
| `5875804` (merge `878a767`) | T6a: las vistas BC sobre imágenes sin comprimir pasan por memoria GPU; escribe aliases storage antes de crear una BC; expande vistas compatibles con más mips/layers. Cambia el recorrido de texturas de piel. Falta explícitamente StorageImageSync tras dispatch y flush antes de escrituras CPU. |
| `45fd5b0` | Si el driver rechaza imágenes BC con storage/block-texel views, reintenta sin esos usos. Puede cambiar cómo se representan los aliases según GPU. |
| `706d686`, `6b7e17c`, `6fffa3e`, `6e86d0a`, `53dec01`, `d751769`, `06d0b6b` | Introducen y refinan DB_ALPHA_TO_MASK. El resultado final vuelve a aplicar dither en todos los draws habilitados por registros, también peleas; la regla de color map queda opt-in. Candidato alternativo: podría descartar fragmentos de un efecto si su alpha o las condiciones emuladas son incorrectos. No se conoce el shader de sangre/sudor para comprobarlo. |
| `631f49f`, `58b2425` | Ejecutan fast-clear en FMASK decompress y limpian imágenes MSAA nunca renderizadas. Afectan capas como tatuajes/resolve; no hay evidencia de que sean las capas de daño. |
| `0b289ce` | Lecturas de CMask como no comprimido (PS4 Pro). Es el único cambio de buffer_cache en el rango: PREUPLOAD no fue reimplementado entre esos tags. |
| `54e5471`, `338222e`, `25998f2`, `aa4bd2b` | Vistas de profundidad R16 por capa/2D, representación D16, escala de bias y Z32 como R32_UINT, liberación de copias. Relevantes para sombras, menos directos para piel. |
| `82b8396` | Fences de subgroup alrededor de stores LDS; posible efecto sobre productores compute. |
| `bc5b87c`, `bc438fe`, `0c15437`, `f34b78e`, `3775520`, `b2f01e7`, `20ce398`, `74ee10e` | NaN/min/max/clamp/recíprocos, EXEC, instancias, VGPR y versión de shader cache. Pueden cambiar cálculo de shaders; sin shader identificado no se pueden excluir individualmente. |
| `250495f` | Emulación de factores MIN/MAX zero/one del blend: candidato si el efecto depende de esos factores. |
| `83b1f3a` | Launcher deja BB_NEO según checkbox: comprobar que el reporte compara el mismo modo PS4/Pro. |

El resto del rango corresponde a saves, icono/portrait, hora local, lenguaje, límite
de FPS, estadísticas y documentación. No se encontró allí un cambio directo del
camino de actualización de texturas de daño.

## Qué hace PREUPLOAD realmente

- `buffer_cache.cpp::PreuploadMode`: 0 apaga; 1 reutiliza memoria ya residente;
  2 admite nuevas residencias (~3 GB extra según el comentario). GuestInPlace fuerza 0.
- `vk_rasterizer.cpp::MapMemory/UnmapMemory` notifican los mappings.
  `NotePreuploadMapping` selecciona direct memory tipo 3 (garlic) con acceso GPU
  mediante `src/runtime_memory.c::runtime_memory_vma_info`. El runtime consulta el
  mapping; no implementa una política específica de sangre/sudor.
- `DrawRecord` llama `Preupload` una vez por frame, presupuesto por defecto 16 MiB.
  Revisa hasta 256 regiones de 4 MiB; espera 10 frames sin writes, aplica backoff
  y busca páginas CPU-modificadas sin modificación GPU. En modo 1 sincroniza sólo
  las partes residentes. No debería sobrescribir datos GPU vigentes si el tracking
  es correcto.
- **También cambia las subidas bajo demanda**: `ObtainBufferForImage` primero toma
  la arena si la región está marcada GPU-modificada, independientemente de PREUPLOAD.
  Con PREUPLOAD activo también permite arena residente + `SynchronizeMemory` para
  datos CPU. Con 0 cae a staging desde guest RAM (`RunGuestCopy`/`CopySparseMemory`).
  Luego `RefreshImage` detilea y sube al VkImage.

Por tanto, **0 no desactiva la subida de una textura dinámica CPU**. Su escritura
debe invalidarla y `UpdateImage`/`RefreshImage` debe cargar los bytes nuevos por
staging. Tampoco desactiva el camino de datos marcados GPU-modificados.
Una diferencia visual con PREUPLOAD=1 apuntaría a una divergencia de coherencia
RAM/arena, invalidación o temporización del snapshot; no demostraría por sí sola
que faltaba la tarea de fondo. PREUPLOAD tampoco garantiza arreglar invalidaciones
perdidas: subir arena no marca automáticamente un VkImage limpio como Dirty.

## Mecanismos concretos que requieren identificar el recurso

1. **Alias BC ya existente tras una escritura storage nueva.** T6a hace writeback
   en `FindImage` dentro de `if (!image_id)`, al crear una BC. Un match exacto o
   el cache de búsqueda puede retornar antes. `FindTexture(Storage)` limpia
   `WrittenBack`; `WriteBackImage` es quien propaga `GpuDirty` a aliases BC.
   `DispatchRecord`/`DispatchIndirectRecord` no hacen StorageImageSync después
   del dispatch. Si sobreviven storage y BC separados, un nuevo write storage
   puede no alcanzar el alias BC previamente creado. Es una limitación concreta
   del código; **no se ha demostrado que sangre/sudor use ese recorrido**, ni que
   sobrevivan ambos aliases en ese caso.
2. **Writes de buffer a subrecursos.** `BindBuffers` invalida imágenes cuando el
   buffer es escrito, pero `InvalidateMemoryFromGPU` sólo considera imágenes con
   base exactamente igual a la dirección del buffer (TODO subresources). Un write
   interior puede dejar el VkImage limpio. Ese mecanismo ya estaba antes del rango;
   no basta para atribuirle la regresión sin demostrar que T6a cambió los aliases.
3. **RAM obsoleta frente a arena vigente.** Si el tracking pierde la autoridad GPU,
   PREUPLOAD=0 puede leer RAM vieja; PREUPLOAD=1 podría conservar bytes en la arena.
   Es una hipótesis de coherencia, no un fallo confirmado. Reactivarlo globalmente
   expone otra vez la piel creada/career que motivó el default actual.

Arreglo de fondo propuesto, condicionado a la captura del recurso: sincronizar el
productor storage hacia su alias BC **también al reutilizarlo**, después del write y
antes del muestreo, respetando orden/barriers y mips/layers. Para writes interiores,
invalidar el rango/subrecurso realmente escrito. Para divergencia RAM/arena,
corregir la autoridad y la invalidación en vez de depender de un upload de fondo.
No se implementó un filtro por formato/dimensión/dirección: hoy no hay un criterio
que distinga el daño de la piel creada y sería especulativo.

**Corrección del contexto:** `GROK_HANDOFF.md` §19 identifica el recurso de §11
(`fs_img3`, R32G32_UINT 512×512×6, ps `86b28487`) como mapa min/max de sombras,
producido por cs `34617c9f`/`aae9f6c1`. No sirve como identificador de sangre/sudor.

## Una prueba manual Linux

Desde este worktree, después de `ufc3-build`. Usa el binario local ya compilado
(`BB_PREBUILT` + `BB_PROBE` evitan que run.sh compile directamente) y los saves del
checkout principal para poder revisar el career existente. No ejecutado por el agente.

```bash
cd /home/myzk/Projects/.ufc3-worktrees/UFC-49
BB_PREBUILT=1 BB_PROBE="$PWD/out/sacredfire" \
BB_GAME_DIR=/home/myzk/Projects/ufc3-native/orig/CUSA06534 \
BB_USER_DIR=/home/myzk/Projects/ufc3-native/user \
BB_PC_MODEL=0 BB_GUEST_IN_PLACE=0 \
BB_PREUPLOAD=1 BB_WRITE_STEP=0 BB_WRITE_WINDOW=1 \
BB_SAVE_LOG=1 BB_LOG_DIR="$PWD/logs/UFC-49-preupload1" \
BB_IMAGE_SYNC_LOG=1 bash ./run.sh
```

Una sola sesión: Quick Fight, golpear repetidamente y revisar sangre/sudor en piel;
luego revisar piel del luchador creado en career (negra, parches o corrupción).
No cambiar otros ajustes gráficos respecto de la sesión con efectos ausentes.
Reportar ambos resultados y conservar el `.log` generado. Los logs actuales son
limitados y no garantizan identificar el recurso sin una captura posterior.

- Vuelven efectos, piel correcta: apoya la hipótesis PREUPLOAD/fuente de upload;
  todavía no justifica cambiar defaults ni garantiza edición repetida correcta.
- Vuelven efectos, piel rota: confirma el tradeoff observado; hace falta resolver
  la coherencia de ambos recursos antes de una solución de release.
- Siguen ausentes: PREUPLOAD=1 no resuelve el caso; revisar productores/aliases y
  alpha-to-mask con el shader real. No encadenar otras pruebas en esta sesión.

Pendiente para cerrar: resultado de esta prueba; si no localiza suficientemente el
problema, captura de una actualización de daño y sus bindings (productor, consumidor,
dirección, formato, subrecursos y flags). El análisis estático no permite elegir
con seguridad un arreglo acotado que conserve la piel creada.
