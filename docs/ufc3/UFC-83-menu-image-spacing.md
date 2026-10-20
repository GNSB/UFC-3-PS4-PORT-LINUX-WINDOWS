# UFC-83 / issue #44: timers ajenos entre pedidos del menú

**Hay una causa de espera demostrada y una carrera corregida en el runtime.
Falta validar el efecto del arreglo en el juego.** No se ejecutó el juego.

Además de las corridas indicadas en el ticket, al revisar `logs/` se encontró
una corrida posterior ya disponible: `20261020_145330.log`, su frames.csv y
`ufc83-delivery.samples` / `.waits` (14:53–14:56). Esa captura permite seguir
quién consume los timers; no fue necesario pedir otra corrida de diagnóstico.

## Cadena causal observada

UI Update despacha a posix → posix solicita un archivo entero a AssetStream →
Load/Unpack/Translate lo completan → posix consulta el estado y duerme 3 ms
antes de volver a consultarlo → otro hilo recibe su timer → posix permanece
dormido hasta recibir un timer ajeno → recién entonces solicita la siguiente GNF.

La tarea se identifica por su vtable `+0x24b60b8`. El polling está en
`+0xe3e050`, consulta `petición[+0x38] & 15` con `+0xe42700`, y termina para
estados >=3. Translate publica 3/4 mediante `+0xe3d170`, store `+0xe3d1cc`;
el resultado está en `petición+0xa0/+0xa8`. No es un fence ni un label de GPU.
El sleep se llama desde `+0xe3e0dc`, agrega un HR timer desde `+0x43e47`
(retorno `+0x43e4c`) y espera desde `+0x43e5f` (retorno `+0x43e64`).
Espera cualquier evento, sin verificar que el ident sea el suyo.

Ejemplo del menú principal, `logs/20261020_145330.log`:

| Tiempo (s) | Hilo | Hecho |
| --- | --- | --- |
| 66.511487 | posix, tid 159592 | Pide `tile_Ad_generic.gnf` |
| 66.511549 | posix | Resolver/stat termina: **0,064 ms** |
| 66.511587 | posix | Agrega timer **1615**, **3000 us**, queue **262144** |
| 66.513099 | /app0 | Read BIG completo: **1,47 ms** |
| 66.514605 | bb:kernel_servi | Dispara 1615, edad **3010,861 us** |
| 66.514622 | **DIME**, tid 160045 | **Consume 1615**, que había pedido posix |
| 66.514631 | DIME | Pide timer **1616**, **50 ms**, misma queue |
| 66.515512–66.561804 | posix, muestras | Petición `0x70059b0870` ya en **estado 4**, buffer listo |
| 66.564646 | bb:kernel_servi | Dispara 1616 |
| 66.564662 | **posix** | **Consume 1616**, tras **53,063 ms** de espera |
| 66.564722 | posix | Pide `pages_fill.gnf` |
| 66.564803 | posix | Pide timer **1619**, **3 ms** |
| 66.567824 | bb:kernel_servi | Dispara 1619, edad **3014,641 us** |
| 66.567841 | **DIME** | Consume 1619 |
| 66.570807–66.662567 | posix, muestras | `pages_fill` ya en **estado 4** |
| 66.667934 | posix | Recibe **1621**, de CallbackManager: **103,121 ms** de espera |

En toda la captura hay **208 timers de 3 ms pedidos por posix** con entrega
registrada: **92 los recibe HangMonitor, 91 CallbackManager y 25 DIME; cero
posix**. Entre 66,4 y 70 s, sus 35 retornos de WaitEqueue tardan una mediana de
**88,660 ms**, rango **53,063–147,537 ms**. No se trata de un timeout redondeado:
los timers de 3 ms disparan puntualmente, pero despiertan a otro consumidor.

El anchor de las muestras es `[6.656063] mono_ns=11997796119964`.
Para correlacionar: `segundos_log = 6.656063 + (mono_ns-anchor)/1e9`.
La precisión es de muestreo (~9 ms), no una transición exacta del estado.

El despacho tampoco explica la pausa: en 14:19, UI Update hace broadcast
`cond=0x2fbe6d30` a 62.306964, posix retorna a 62.306984 y abre la GNF a
62.307035. Sitios del productor: `+0x1a3933a,+0x1daa137,+0x1daa35f`.

## Carrera del runtime y arreglo

EAThread guarda una identidad derivada del primer campo del handle ScePthread:

- Después de crear un hilo: `+0x46ee9` carga el handle desde el registro de
  EAThread; `+0x46ef1` lee `*handle`; `+0x46ef4` lo guarda en ese registro.
- Al dormir: `+0x43ddb` lee TLS y `+0x43de7` busca el registro EAThread en
  TLS[-0x18]. Si falta, `+0x43e89` llama `scePthreadSelf` (PLT/GOT
  `+0x1dfa3e0/+0x25c6050`, import 99 `aI+OeCz8xrQ`), y `+0x43e8e` lee
  `*self`. Busca ese valor entre los 128 registros de `+0x297c350`.
- Si no lo encuentra, usa la **cola compartida de respaldo** en `+0x297c338`.
  Los registros EAThread sí contienen su propia cola en +0x90.

En `src/runtime_thread.c`, el primer campo de GuestThread es `tcb`.
Antes, `create` publicaba el handle y retornaba, pero el hijo inicializaba ese
campo recién en `host_start → attach`. El creador podía guardar **cero**;
cuando el hijo consultaba su identidad encontraba su TCB **no nulo**. Es una
carrera real de publicación, suficiente para romper esa búsqueda.

Se separó `prepare_tcb` de `attach`: la creación reserva e inicializa TCB/TLS
**antes de publicar el handle y antes de pthread_create**. El hijo publica
GS/TEB y su identidad local usando ese mismo TCB, sin reemplazarlo. El hilo
principal y los hilos host conservan la inicialización al adjuntarse.

`tests/test_runtime.c` verifica con ocho hilos que el campo ya sea no nulo
cuando retorna create, único entre hilos, igual a lo que observa self en el
hijo, y estable después de join. También existe `--thread-identity-probe`.
Quitando sólo la preparación anterior a publish, `ufc3-build --test` falla
con **exit 134** en `identity.identity && identity.identity!=previous`.
Con el arreglo pasa. La prueba negativa no generó un core (ulimit -c 0).

La pila de CallbackManager en la corrida de 14:39 incluye `+0x4722e`,
dentro del wrapper EAThread `+0x471a0`. Su creador `+0x470c0` llama al
mismo `+0x46b00`, que registra la identidad del handle. Por tanto, al menos
uno de los consumidores ajenos pasa por el camino de registro afectado.

**Límite:** los logs no contienen los valores de todos los registros EAThread
al crear cada hilo. Por tanto, demuestran la espera por timers ajenos y el
análisis/test demuestra la carrera, pero no prueban que ésta explique cada
hilo en la cola compartida. Los hilos creados fuera de EAThread también pueden
usar legítimamente el respaldo. La prueba manual debe confirmar si retirar
los hilos afectados por la carrera elimina el espaciado. No se modifica la
semántica general de equeue ni se asignan eventos a su creador: las colas
permiten consumidores diferentes del productor.

Reproducción estática sin ejecutar guest:

```bash
python tools/re/ufc83_asset_task.py out/eboot.elf --disassemble
```

ELF 1.14 SHA256:
`08a4ffbc9b69934bfa9443fd116e49255c69d1c10834055c9a3f1f551399c18f`.
El script valida instrucciones y extrae PT_LOAD con su dirección virtual;
los offsets del archivo BBPROBE5 `boot-linked.bin` no son direcciones guest.

## Trazado que perturbó la corrida de 14:39

El log fallido termina a 40,474 s, antes de press start, con **76.412 líneas
que contienen Wait/Cond trace**. Las últimas muestras muestran Load dentro de
`_IO_printf → _IO_file_write` a mono_ns=11219455112792, Unpack esperando el
lock de stdout en `runtime_wait_note` a 11219461034576 y otro hilo en
`EqueueInternal::TraceEvent → _IO_printf` a 11219455962098.
Esto demuestra contención introducida por el trazado; no acredita por sí
solo un deadlock ni explica todo el tiempo sin progreso.

La revisión previa ya separó BB_WAIT_TRACE_CALLS/BB_COND_TRACE de la selección
de hilos y bajó el sampler a una señal cada 8 ms por tid, escalonadas.
Esta revisión elimina además el I/O de `EqueueInternal::TraceEvent`, que aún
podía imprimir reteniendo el mutex de eventos y desde el servicio de timers.
Ahora copia registros a un buffer acotado de 8192 entradas, sin esperar si
hay contención o se llena. El reporte de frames escribe las copias fuera de
los mutex del equeue; `BB_EQUEUE_LOG` permite un archivo separado de stdout.
Los timestamps pertenecen al productor, no al vaciado. Las pérdidas se
reportan como `Equeue trace incomplete: dropped=...`; no deben interpretarse
ident faltantes como eventos perdidos si aparece ese aviso. Se puede perder
la cola final del trazado al salir, porque se vacía con el reporte periódico.
El trazado sigue teniendo costo; no se promete perturbación cero.

## Una sola prueba manual del arreglo

Desde este worktree, mismos ajustes y sin borrar temp0:

```bash
env -u BB_COND_TRACE -u BB_WAIT_TRACE_CALLS \
BB_SAVE_LOG=1 BB_FRAME_STATS=1 BB_FILE_TRACE=2 BB_EQUEUE_TRACE=1 \
BB_EQUEUE_LOG="$PWD/logs/ufc83-fixed.equeues" \
BB_WAIT_THREAD_TRACE='posix,UI Update' BB_SAMPLE_THREAD='posix' \
BB_SAMPLE_LOG="$PWD/logs/ufc83-fixed.samples" \
BB_WAIT_LOG="$PWD/logs/ufc83-fixed.waits" bash ./run.sh
```

Menú hasta ver imágenes → Fight Modes → volver y esperar las imágenes →
Fight Now hasta ver retratos → volver → salir. Entregar log automático,
frames.csv y los tres archivos ufc83-fixed; indicar si persisten los ~3 s.
Si no llega a press start, interrumpir esa misma corrida y entregar lo generado.

Criterio: desaparece o se reduce el intervalo 60–100 ms entre GNF y las
peticiones terminadas dejan de quedar bloqueadas esperando timers ajenos.
Si sigue ocurriendo, queda pendiente determinar qué hilos son externos a
EAThread y qué garantía de despertar necesita ese uso compartido; no se
considera resuelto por el solo hecho de compilar.

## Alcance y verificación

No se encontró una limpieza propia de temp0 en runtime_file_configure,
run.sh o launcher. La ausencia inicial de cap_* no identifica quién los
quitó. Las GNF del menú son independientes de las capturas SPG y el retraso
reaparece sin reiniciar; no hay evidencia para cambiar persistencia,
`25ff544` ni BB_READBACK_LINEAR_ONLY. Este cambio no altera captura/readback,
labels, write windows ni protecciones de Career/post-pelea (UFC-27/UFC-47).
Esas pantallas no se ejecutaron: no se afirma validación visual.

**UFC-81 sigue valiendo como optimización de picos de upload/protección en
Deck; el A/B en PC no mejoró esta latencia y no resuelve issue #44.**

Validación final:

- `ufc3-build --test`: **exit 0**, checks nativos y **42 tests Python OK**.
  Log `/tmp/ufc83-final-build.log`.
- Regresión sin preparar el TCB antes de publish: **exit 134** en el nuevo
  assert; se restauró el arreglo y se recompiló. Log
  `/tmp/ufc83-identity-negative.log`.
- `BB_EQUEUE_TRACE=1 BB_WAIT_THREAD_TRACE=runtime-test out/runtime-test
  --equeue-timer-probe`: exit 0, **20/20** timers de 3 ms; mediana **3,033 ms**,
  máximo **3,066 ms**. Sin guest ni dispositivo Vulkan. Este probe monohilo
  verifica el camino de timer con trazado bufferizado; no simula la competencia
  de la sesión. Log `/tmp/ufc83-final-equeue-probe.log`.
- Script estático con `--disassemble`: exit 0. Log
  `/tmp/ufc83-final-static.log`.
- `git diff --check`: OK.

El submódulo `gpu/third_party/fsr-vulkan` ya estaba modificado y no se tocó.
No se ejecutó el juego, ni se hicieron commits o cambios de rama.
