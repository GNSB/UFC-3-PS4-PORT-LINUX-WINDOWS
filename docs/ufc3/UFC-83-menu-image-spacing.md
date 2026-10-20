# UFC-83 / issue #44: tarea que bloquea a posix

**Estado: identificada estáticamente la tarea y el hilo que publica su terminación.
Pendiente medir qué etapa consume los 60–100 ms; no hay arreglo de latencia demostrado.**
La entrega agrega instrumentación para una sola corrida de la cadena completa.
No se ejecutó el juego. No se modificaron readbacks, labels, write windows,
persistencia, ni las protecciones de Career/post-pelea (UFC-27/UFC-47).

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
La revisión del Jefe establece que los timers son puntuales: por ello se sigue
al productor del estado, en lugar de pedir otra corrida dedicada al timer.

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

## Cambios de esta entrega

- `src/runtime_kernel.c`: BB_SAMPLE_THREAD acepta una **lista de prefijos**,
  selecciona **todos los tid coincidentes** y redescubre threads cada segundo.
  Considera la truncación de comm a 15 caracteres en Linux. Antes sólo elegía un
  tid y no podía observar ambos Unpack/Translate. El sampler tiene nombre propio
  para no seleccionarse al heredar el nombre del padre.
- Cada muestra incluye tid, timestamp monotónico, registros rbx/r12/r14/r15,
  pila host simbolizada y retornos guest. La cola de muestras tiene exclusión
  **sin esperar** entre signal handlers y reporta muestras descartadas. No hay
  stdio, simbolización ni locks bloqueantes dentro del handler. Los agregados
  `Thread samples` combinan los hilos; el archivo detallado distingue cada tid.
- En BB_SAMPLE_LOG se agregan observaciones **`asset-task`**: puntero, estado,
  buffer, tamaño y ruta. Sólo interpreta candidatos tras validar la vtable
  concreta +0x24b60b8. Durante el polling recupera además el r15 guardado en
  el frame que retorna a +0x1bab6f0. Lee con process_vm_readv: un puntero inválido
  falla sin desreferenciarlo. Son snapshots de diagnóstico, no transiciones
  exactas ni lecturas atómicas del objeto completo. No se escribe memoria guest.
  La captura de tareas es específica de este ELF; otras revisiones simplemente
  no producirán esa evidencia si sus vtables/retornos cambian.
- `tests/test_runtime.c`: smoke optativo con un thread activo y dos workers con
  nombres largos duplicados; comprueba selección múltiple, tid distintos,
  muestras de ambos y preservación de errno frente a señales.
- `tools/re/ufc83_asset_task.py`: reproducción estática y validación de offsets.
- Se reutilizan BB_FILE_TRACE=2 (tiempo de resolución), BB_EQUEUE_TRACE=1 y
  BB_WAIT_THREAD_TRACE de las entregas anteriores.

## Una sola corrida manual

Desde este worktree, mismos ajustes y sin borrar temp0:

```bash
BB_SAVE_LOG=1 BB_FRAME_STATS=1 BB_FILE_TRACE=2 BB_EQUEUE_TRACE=1 \
BB_SAMPLE_THREAD='posix,AssetStream::,UI Update,RenderThread' \
BB_SAMPLE_LOG="$PWD/logs/ufc83-task.samples" \
BB_WAIT_THREAD_TRACE='posix,AssetStream::Lo,AssetStream::Un,AssetStream::Tr,UI Update,RenderThread' \
BB_WAIT_LOG="$PWD/logs/ufc83-task.waits" bash ./run.sh
```

Menú hasta ver imágenes → Fight Modes → volver y esperar → Fight Now hasta ver
retratos → volver → salir. Entregar el log automático, frames.csv,
`ufc83-task.samples` y `ufc83-task.waits`. La lista de waits usa los nombres
truncados exactos de Linux; la lista del sampler usa prefijos.

En la misma ventana entre GNF, seguir `asset-task task=... path=...` en posix;
clasificar estado 0 (en cola), 1 (carga/bloques) o 2 (Translate/stream). Correlacionar
las muestras **por tid** de los cinco workers con ese intervalo y puntero. Una
espera del stream en Translate y CPU activa en Unpack identifica descompresión;
Load bloqueado en imports identifica la lectura/servicio; Translate en allocator,
callback o runtime identifica esa dependencia concreta. Los stacks y waits de
UI/Render cubren eventuales dependencias de esos hilos. El equeue trace permite
verificar los sleeps dentro de la misma corrida, sin pedir otra sólo para timers.
El sampler es de ~9 ms por hilo bajo carga normal, no una medición instrucción
por instrucción; pérdidas quedan explícitas. Es una corrida diagnóstica: todavía
no se promete una reducción de los 3 s ni que una muestra capture una transición
muy corta.

## Alcance pendiente y UFC-81

La revisión anterior de runtime_file_configure/run.sh/launcher no encontró una
limpieza propia de temp0; la ausencia inicial de cap_* no identifica quién los
quitó. Las GNF del menú son independientes de la captura SPG y el retraso reaparece
al volver al menú sin reiniciar. No hay evidencia para modificar persistencia,
`25ff544` o BB_READBACK_LINEAR_ONLY. Fight Now requiere su correlación en la
misma corrida; no se extrapola el estado del menú a todos los retratos.

**UFC-81 sigue valiendo como optimización de picos de upload/protección en Deck;
el A/B en PC no mejoró la latencia y no resuelve issue #44.**

## Verificación final

- `ufc3-build --test`: **exit 0**, incluidos los checks nativos y **42 tests
  Python OK**. Log `/tmp/ufc83-task-build.log`.
- `out/runtime-test --sample-trace-smoke`: **exit 0**; ambos workers duplicados
  tienen muestras, tids distintos y errno preservado. La enumeración excluye
  las entradas `.`/`..` de proc y el propio sampler.
  Log `/tmp/ufc83-task-sampler-smoke.log`.
- `python tools/re/ufc83_asset_task.py out/eboot.elf --disassemble`: **exit 0**;
  valida gestor, vtable, strings, retorno del polling y escritura de estado.
- `git diff --check`: OK. No se ejecutó el juego ni se hicieron commits.

El submódulo `gpu/third_party/fsr-vulkan` ya figuraba modificado al empezar;
esta entrega no lo toca. Los cambios propios son los cuatro archivos indicados
(runtime, test, herramienta estática e informe).
