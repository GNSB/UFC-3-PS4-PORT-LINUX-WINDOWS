# UFC-83 / issue #44: tarea que bloquea a posix

**Estado tras la corrida fallida de 14:39: issue #44 sigue abierto.**
La petición y el productor están identificados, pero las nuevas muestras contradicen
que la pausa observada sea necesariamente la duración de AssetStream. Hay peticiones
ya terminadas mientras su solicitante sigue dentro del sleep por equeue. Se corrige
el diagnóstico y se reduce la instrumentación; no hay arreglo de latencia demostrado.
No se ejecutó el juego ni se tocaron readbacks, labels, write windows, persistencia
ni las protecciones de Career/post-pelea (UFC-27/UFC-47).

## Hallazgo de 14:39 y límites de la corrida

Fuentes: `logs/20261020_143957.log`, `logs/ufc83-task.samples` y
`logs/ufc83-task.waits`. El log tiene exactamente **76.412 líneas Wait/Cond trace**
y termina a 40.474 s; no llegó a press start ni al menú objeto del ticket.
Además registró **22.147 eventos de FlipManager**, frente a 1.152 registros
de la cola de timers compartida 262144. El comando anterior era demasiado
invasivo y no debe repetirse.

La instrumentación imprimía en cada handoff reteniendo mutex guest (cond_wait)
o el mutex global del servicio de semáforos (wait_count). Las últimas muestras
incluyen `AssetStream::Lo` en `_IO_printf → _IO_file_write` a
`mono_ns=11219455112792` y `AssetStream::Un` bloqueado en
`__lll_lock_wait_private → _IO_printf → runtime_wait_note` a
`11219461034576`. Esto acredita contención introducida por el diagnóstico,
**no prueba por sí solo un deadlock ni la causa de la falta de progreso**.
Entre aproximadamente 15 y 35 s predominan workers en cond_timedwait; tampoco
se puede atribuir todo ese intervalo a stdout.

El anchor del archivo de muestras es `[6.873443] mono_ns=11185858239728`.
Restándolo se correlacionan snapshots con timestamps del log (precisión aproximada):

| Tiempo (s) | Hilo/sitio | Observación |
| --- | --- | --- |
| 12.095889 | posix, +0x43e4c | Agrega timer **224**, **3000 us**, queue **262144** |
| 12.097834 | posix, +0x43e64 ← +0xe3e0dc ← +0x1bab6f0 | `Zinc.swf`, petición `0x7005bf5df0`, estado **0** |
| 12.099432 | bb:kernel_servi | Dispara **224** tras **3523.677 us** |
| 12.108394 | posix, misma pila | Misma petición, estado **4**, buffer `0x70174179a0`, 203074 bytes |
| 12.118623–12.255450 | posix, misma pila | Sigue dentro de WaitEqueue, estado **4** en todas las muestras |
| 12.258456 | bb:kernel_servi | Dispara timer **222**, de **250 ms** |
| 12.258469 | posix, +0x43e64 | Recibe **222**, tras **162558.145 us** de wait |
| 12.330812 | posix, +0x43e4c | Agrega **228**, **3000 us**, misma queue |
| 12.333832 | bb:kernel_servi | Dispara **228** tras **3013.211 us** |
| 12.386266 | posix, +0x43e64 | Recibe **229** (timer de 50 ms), espera **55443.422 us** |

De los 93 snapshots asset-task, **86 tienen estado 4, cinco 1, uno 0 y uno 2**.
Son observaciones, no porcentajes de tiempo del juego sin instrumentación.
También `betaWarning.gnf` está en estado 4 desde **38.846730** hasta al menos
**38.939962**, con la misma pila de sleep. El desfase no es exclusivo de SWF.

**Cadena revisada:** UI Update → posix → petición AssetStream → Translate publica
estado 3/4 → posix aún puede quedar en +0x43d30/WaitEqueue antes de volver a consultar.
La publicación del timer es puntual en los ejemplos; **la entrega al solicitante
no lo es**. El anterior probe monohilo de 3 ms no cubre este caso compartido.
No hay evidencia para cambiar GPU/EOP ni para decir que el juego tarda 70 ms
por descompresión. Tampoco hay una comparación equivalente con PS4.

El sleep guest explica por qué hay una cola compartida: +0x43ddb lee TLS,
+0x43de7 obtiene el objeto EAThread de TLS[-0x18]; si es nulo, +0x43e89 busca
el hilo en 128 registros de +0x297c350. A +0x43ebb selecciona el timer de
respaldo **+0x297c338** si no lo encuentra. Registra un ident creciente en
+0x43e42 y espera **cualquier evento** en +0x43e5f (num=1, timeout=NULL).
No comprueba que el ident recibido sea el solicitado. Los threads externos a
EAThread pueden competir por ese respaldo. **Esto no demuestra un fallo de TLS,
ni autoriza a privatizar colas o entregar eventos según su creador:** la API de
colas admite consumidores distintos del productor.

El trazado anterior sólo registraba `delivered` cuando el consumidor tenía un
nombre seleccionado; los `trigger` sí se imprimían para toda la cola marcada.
Por tanto, no se sabe quién consumió 224/228. La entrega ahora registra a **todos
los consumidores de una cola marcada**, incluyendo nombres/tid de los no
seleccionados. Ésa es la evidencia que faltaba para distinguir consumo por otro
hilo, demora efectiva de despertar y perturbación por trazado.

## Evidencia temporal existente

Fuentes: `logs/20261020_141917.log`, `logs/ufc83-host.samples`,
`logs/ufc83-host.waits`. Offsets guest relativos a `0x800000000`.

- **UI Update, tid 138339**, despacha con broadcast `cond=0x2fbe6d30`, sitios
  `+0x1a3933a,+0x1daa137,+0x1daa35f`. A **62.306964** despierta a
  **posix, tid 138354**: retorna a **62.306984**, abre la GNF a **62.307035**.
  El envío y el despertar no explican una pausa de 70 ms por textura.
- Entre `tile_Ad_generic` a **62.560222** y `pages_fill` a **62.640993** hay
  **80.771 ms**. Resolver Ad cuesta **0.045 ms**, incluidos **0.031 ms** de
  fix_case. La lectura BIG llega a **62.560362**, cuesta **0.03 ms**.
  Las **8/8 muestras** entre **62.568172–62.633096** están en
  `+0x43e64 < +0xe3e0dc < +0x1bab6f0 < +0x19d96cc < +0x19fbc79 < +0x1da9ce2`.
- `pages_fill` → `ras_rl`: **62.640993 → 62.711394**, **70.401 ms**;
  ruta **0.043 ms**, lectura **0.01 ms**, 8/8 muestras en el mismo polling.
  Al volver al menú: **70.587996 → 70.690500**, 11/11 muestras iguales.
- Totales: 8206 muestras, 6071 en condición de cola vacía y 2090 en polling.
  Esos totales no miden la duración de cada timer ni la de cada tarea.

`+0x43d30` recibe el double 3.0, lo convierte a 3,000,000 ns, registra un
HR OneShot y espera su equeue. La cola es **EAThread Timer Queue**, filter=-15;
la dispara el servicio de kernel, no un EOP. El probe previo sin guest entregó
20/20 timers, mediana 3.040 ms y máximo 3.076 ms. No mide carga guest.
La puntualidad del callback no implica entrega puntual al solicitante: véase
la corrección con tareas ya terminadas y consumidores de la cola compartida arriba.

## Análisis estático: qué operación es y quién la completa

ELF 1.14 `out/eboot.elf`, SHA256
`08a4ffbc9b69934bfa9443fd116e49255c69d1c10834055c9a3f1f551399c18f`.
Reproducción sin ejecutar guest:

```bash
python tools/re/ufc83_asset_task.py out/eboot.elf --disassemble
```

El script verifica las tablas y las instrucciones citadas y extrae el PT_LOAD
con su dirección virtual. El archivo `boot-linked.bin` tiene cabecera BBPROBE5:
**sus offsets de archivo no son direcciones guest**.

1. `posix` consume tareas en `+0x1da9dc0`; antes de avanzar llama la operación
   virtual anterior (`+0x1da9cdf`, retorno `+0x1da9ce2`). La cadena observada
   alcanza `+0x1bab670`. Allí arma el descriptor (flags 0xff, campo +0x18 del
   descriptor = puntero de `objeto+0x10`), llama `+0xe3c600` a `+0x1bab6db`
   y conserva el resultado en **r15**. A `+0x1bab6eb` pasa ese puntero a
   `+0xe3e040`, retorno **+0x1bab6f0**.
2. `+0xe3c600` usa el gestor global `+0x25e2148` y salta a `+0xe3a870`.
   Este construye una **petición de archivo completo**, de **0xb0 bytes**,
   con vtable **+0x24b60b8**. Guarda el descriptor en +0x48..+0x9f, limpia
   el resultado +0xa0/+0xa8 y la encola con la operación virtual +0x38 del
   gestor. No es una creación de imagen Vulkan ni una captura de render target.
3. El polling `+0xe3e050` usa otro gestor global, **+0x25e2158**, inicialmente
   `+0x25e2150`, vtable **+0x24b6108**. La consulta virtual **+0x30** es
   **+0xe42700**: `lfence; mov eax,[rsi+0x38]; and eax,15; ret`.
   **rsi es la petición**, conservada en r12 por el waiter. Por tanto, la
   condición exacta es **`(peticion->campo_38 & 15) >= 3`**. No consulta un
   label ni el estado de una textura del host.
4. `+0xe3f1e0` crea los hilos de AssetStream. Los strings y entradas son:

   | Etapa | String en ELF | Entrada | Trabajo |
   | --- | --- | --- | --- |
   | Load | +0x1f19b77 `AssetStream::Load` | +0xe41990 | +0xe3f740 |
   | Unpack | +0x1f19b89 `AssetStream::Unpack` | +0xe41af0 | +0xe40b20 |
   | Translate | +0x1f19b9d `AssetStream::Translate` | +0xe41c40 | +0xe40800 |

   Son los mismos nombres que registra el log al crear los threads. Load
   retira la petición de su cola y escribe **estado 1** en **+0xe3fa7a**.
   Reconoce chunks `chunkzip`, `chunkref`, `chunlzma`, `chunklzx` y entrega
   bloques a las colas de Unpack/Translate. El camino de error también puede
   entregar un bloque vacío mediante +0xe405f0.
5. **Translate** recupera la petición desde `bloque+0x18` en **+0xe40856**,
   la conserva en **rbx** y escribe **estado 2** en **+0xe40872**. Construye
   un stream en +0xe42c00. A **+0xe4095a** llama `peticion->vtable+0x20`,
   que para esta petición es **+0xe3c6f0**; si ya tiene un resultado usa +0x28.
   +0xe3c6f0 obtiene el tamaño del stream, reserva el buffer, lo guarda en
   **peticion+0xa0**, guarda tamaño en **+0xa8**, y lee ese stream por su
   método +0x28 en **+0xe3c7aa**. Si existe, llama el callback +0x90.
6. Al retornar, Translate calcula **3 + booleano_del_resultado** en
   **+0xe40a65**, y llama **+0xe3d170** en **+0xe40a9b**, con rdi=petición,
   esi=1 y edx=3/4. Esta función publica el estado en **+0xe3d1cc** preservando
   los otros bits de +0x38, bajo el mutex global `+0x2a9e558`.
   El polling acepta tanto 3 como 4; no hace falta asignarles nombres no
   presentes en el binario. Después posix consume el buffer +0xa0 y avanza.

**Cadena acreditada:** UI Update → cola de posix → petición de archivo completo
→ AssetStream::Load → Unpack cuando corresponde → Translate → publicación de estado → siguiente
pedido de posix. **Translate es el productor final**, pero su lectura del stream
puede esperar bloques de Load/Unpack; muestrear sólo Translate tampoco bastaría.
La asignación de callbacks/streams permite llamadas virtuales adicionales: no
se atribuye su costo a CPU, AIO o GPU sin observarlas.

El propio guest serializa estas peticiones y elige el polling de 3 ms. Esto no
demuestra que cada petición tarde 70 ms en PS4. Sin muestras de esos productores
no se puede afirmar un tick de UI, un retraso de EOP, ni un bug de scheduler.

## Cambios tras la corrida fallida

- `src/runtime_kernel.c`: el sampler envía una señal cada **8 ms por tid**, y
  guarda el detalle en cada señal, en vez de enviar cada 0.5 ms y guardar una
  de cada 16. Reduce aproximadamente 16 veces las interrupciones conservando
  la cadencia de detalle. Las señales se escalonan entre tid para evitar que
  sus handlers compitan simultáneamente y dejen hilos sin muestras. El smoke
  detectó ese problema al bajar la frecuencia y pasa tras escalonarlas. Mantiene
  snapshots validados y contador de pérdidas.
- Los registros Wait por llamada requieren además **BB_WAIT_TRACE_CALLS=1**.
  `src/runtime_mutex.c`: los registros Cond requieren **BB_COND_TRACE=1**.
  Seleccionar un hilo para equeue ya no activa ambos logs de alto volumen.
  Los agregados BB_WAIT_LOG siguen disponibles. Ambas opciones se dejan
  desactivadas en la prueba propuesta.
- `gpu/shadps4/core/libraries/kernel/equeue.cpp`: una cola seleccionada registra
  todos sus consumidores y sus altas de timer, para identificar quién recibe
  cada evento; incluye consumidores que ya esperaban cuando se marcó la cola. La selección inicial se limita a posix/UI Update y no marca
  FlipManager por seleccionar RenderThread. No cambia la semántica de eventos.
- `tools/re/ufc83_asset_task.py`: amplía la reproducción estática al sleep y
  la selección de la cola de respaldo.

No se presenta esto como arreglo del cuelgue: disminuye su perturbación conocida,
pero no se ha reproducido ni descartado un bloqueo bajo carga guest.

## Una sola corrida manual

Desde este worktree, mismos ajustes y sin borrar temp0. El comando elimina las
opciones de alto volumen aunque estén heredadas del entorno:

```bash
env -u BB_COND_TRACE -u BB_WAIT_TRACE_CALLS \
BB_SAVE_LOG=1 BB_FRAME_STATS=1 BB_FILE_TRACE=2 BB_EQUEUE_TRACE=1 \
BB_WAIT_THREAD_TRACE='posix,UI Update' \
BB_SAMPLE_THREAD='posix,AssetStream::,UI Update' \
BB_SAMPLE_LOG="$PWD/logs/ufc83-delivery.samples" \
BB_WAIT_LOG="$PWD/logs/ufc83-delivery.waits" bash ./run.sh
```

Menú hasta ver imágenes → Fight Modes → volver y esperar → Fight Now hasta ver
retratos → volver → salir. Entregar el log automático, frames.csv y ambos
archivos ufc83-delivery. Si vuelve a no llegar a press start, interrumpir esa
misma corrida y entregar lo generado; no repetirla con más variables.

En cada intervalo GNF correlacionar la petición/estado de posix con las muestras
por tid de Load/Unpack/Translate. Si ya está completa, seguir el ident de su timer
entre `hrtimer-add`, `trigger` y **todos** los `delivered` de la misma queue.
Esto captura la dependencia AssetStream y el destino de los eventos en una sola
corrida. El sampling no entrega transiciones exactas; sus pérdidas son explícitas.

## Alcance pendiente y UFC-81

La revisión anterior de runtime_file_configure/run.sh/launcher no encontró una
limpieza propia de temp0; la ausencia inicial de cap_* no identifica quién los
quitó. Las GNF del menú son independientes de la captura SPG y el retraso reaparece
al volver al menú sin reiniciar. No hay evidencia para modificar persistencia,
`25ff544` o BB_READBACK_LINEAR_ONLY. Fight Now requiere su correlación en la
misma corrida; no se extrapola el estado del menú a todos los retratos.

**UFC-81 sigue valiendo como optimización de picos de upload/protección en Deck;
el A/B en PC no mejoró la latencia y no resuelve issue #44.**

## Verificación de esta revisión

- `ufc3-build --test`: **exit 0**, checks nativos y **42 tests Python OK**.
  Log `/tmp/ufc83-review-final-build.log`.
- `out/runtime-test --sample-trace-smoke`: exit 0 tras escalonar señales;
  ambos workers duplicados tienen muestras y errno se preserva.
  Log `/tmp/ufc83-review-sampler.log`.
- `python tools/re/ufc83_asset_task.py out/eboot.elf --disassemble`: exit 0.
  Log `/tmp/ufc83-review-static.log`.
- `git diff --check`: OK.
El submódulo `gpu/third_party/fsr-vulkan` ya figuraba modificado al empezar y no
se tocó. No se ejecutó el juego, ni se hicieron commits o cambios de rama.
