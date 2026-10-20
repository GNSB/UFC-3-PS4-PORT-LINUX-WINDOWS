# UFC-83 / issue #44: espera del cargador, corrida 14:19

**Estado: identificada la espera como polling con HR timers; causa de su duración
pendiente. No hay un arreglo de latencia acreditado.** La pila observada no espera
un EOP ni un flip. Cambiar el flush a partir de esta pila no está justificado.
No se ejecutó el juego, ni se tocaron labels, write windows, readback, persistencia
o las protecciones de Career/post-pelea (UFC-27/UFC-47).

## Evidencia y cadena causal identificada

Fuentes: `logs/20261020_141917.log`, `logs/ufc83-host.samples`,
`logs/ufc83-host.waits`, ELF 1.14 `out/eboot.elf` y tabla de imports
`out/boot.bin`. Los offsets guest son relativos a 0x800000000.

1. **UI Update (tid 138339)** encola trabajo y hace broadcast de
   `cond=0x2fbe6d30`, sitios `+0x1a3933a,+0x1daa137,+0x1daa35f`.
   A 62.306964 despierta a **posix (tid 138354)**, que sale de la condición
   a 62.306984 y consulta la primera GNF a 62.307035. La ráfaga de envíos
   continúa mientras posix trabaja. El despachador no está esperando 70 ms
   entre cada envío; esta señal tarda unos 20 microsegundos en despertar.
2. Posix consume la cola en `+0x1da9dc0`; al pasar a la siguiente tarea
   llama a una operación virtual de la tarea anterior en `+0x1da9cdf`,
   retorno **+0x1da9ce2**. Esa operación entra por `+0x19fbc74 ->
   +0x19d95a0`, retorno **+0x19fbc79**; continúa por `+0x19dcaf0`
   hacia **+0x1bab670**. Esta última crea/encola una operación en
   `+0xe3c600 -> +0xe3a870` y espera su estado mediante `+0xe3e040`.
   Los nombres/tipo exacto de esa operación no están en los logs.
3. **+0xe3e050** consulta el estado por la función virtual `vtable+0x30`.
   Si aún no llegó a 3, ejecuta el bucle `+0xe3e0d0..+0xe3e0eb`:
   almacena el double **3.0** (`0x4008000000000000`), llama a
   **+0x43d30**, retorna en **+0xe3e0dc**, reconsulta y repite.
4. **+0x43d30 es un sleep en milisegundos**, no una espera de gráficos:
   multiplica por **1000000.0** (constante en +0x1e05028), convierte a
   nanosegundos, divide por 1000000000 y arma un timespec. Con 3.0 pide
   `sec=0, nsec=3000000`. Usa la cola del thread (TLS), incrementa el ident,
   llama a **sceKernelAddHRTimerEvent** en +0x43e47 y a
   **sceKernelWaitEqueue** con timeout NULL en +0x43e5f, retorno
   **+0x43e64**. Se verificaron los PLT/GOT: +0x1dfa0c0/GOT+0x25c5ec0
   resuelve R74tt43xP6k (AddHRTimer); +0x1dfa160/GOT+0x25c5f10 resuelve
   fzyMKs9kim0 (WaitEqueue).
5. En el host esa petición registra **filter=-15 (HrTimer), OneShot**.
   Como 3 ms supera el umbral de 1.2 ms de `equeue.cpp`, se programa en
   `boost::asio::steady_timer`; `bb:kernel_service` ejecuta el callback
   `HrTimerCallback -> TriggerEvent -> notify_one`. No depende de un submit
   Vulkan ni de ProcessDownloadImages. La implementación usa un io_context
   con work guard y run() continuo, no un poll por frame.

La cola se llama **EAThread Timer Queue**: su constructor +0x46a12 carga ese
string (+0x1e05246), +0x46a31 llama a sceKernelCreateEqueue y escribe el handle
en thread+0x90, el mismo campo usado por +0x43dfe/+0x43e3f. El fallback global
se crea con igual nombre en +0x47782/+0x477a5. El **handle numérico y el ident
concreto** no quedaron registrados en las muestras ni en `.waits`: no se pueden
recuperar desde una dirección de futex. El trace nuevo distingue las instancias
y registra cada ident directamente.

### Correlación temporal, no sólo totales de sesión

La primera línea de samples da el ancla: tiempo 21.524725 corresponde a
`mono_ns=9960169548506`. Se convierte cada muestra usando esa diferencia.

| Ventana de posix | Resolución de ruta | Lectura BIG | Muestras entre pedidos |
| --- | ---: | ---: | --- |
| Ad 62.560222 → pages_fill 62.640993 (80.771 ms) | Ad 0.045 ms, fix_case 0.031 ms | 62.560362, 0.03 ms | 8/8 en +0x43e64 / +0xe3e0dc (62.568172–62.633096) |
| pages_fill 62.640993 → ras_rl 62.711394 (70.401 ms) | pages_fill 0.043 ms | 62.641124, 0.01 ms | 8/8 en la misma pila (62.643428–62.708919) |
| Ad al volver, 70.587996 → 70.690500 | 0.058 ms | lectura inmediata | 11/11 en la misma pila (70.596699–70.690315) |

En toda la sesión: **8206 muestras**, 6071 en la condición de cola vacía,
2090 con los retornos +0x43e64/+0xe3e0dc y 45 restantes. El número de muestras
no es el número de waits: un sampler a ~9 ms puede observar varios sleeps de
3 ms distintos con idéntica pila. Por tanto, **no demuestra que cada timer
esté tardando 70–100 ms** ni que una sola espera dure los ~18 s acumulados.
Tampoco demuestra que el estado pendiente sea GPU; la condición del bucle es
una consulta virtual a una operación del guest, no un label/fence en esta pila.

UI Update espera además en otra condición (`0x2f3797b0`), señalada por SimThread
cada ~33.33 ms; por ejemplo 62.547993 → retorno 62.548011. Esto es distinto de
la condición del cargador. Los EOP agregados siguen sin pendientes (p.ej.
77809 decodificados/escritos, línea 16043). No es una medición de latencia de EOP,
pero tampoco apoya tocar las protecciones por labels tardíos.

## Qué falta y por qué no se cambia la semántica todavía

Hay dos explicaciones aún compatibles: timers entregados tarde, o sleeps de 3 ms
normales repetidos mientras la operación consultada permanece pendiente. Los
logs existentes no registran cada alta/disparo/retorno de timer. No permiten
comparar sus 3 ms pedidos con la duración real, ni identificar quién publica el
estado final de esa operación. No corresponde afirmar que sea el ritmo normal
de PS4 o que haya una textura por tick de UI.

Si los timers son puntuales, el próximo eslabón es la operación creada por
+0x1bab670/+0xe3a870 y su productor de estado. Eso requiere seguir esa operación,
no adelantar eventos GPU. No se implementó un caché negativo: las mediciones
directas vuelven a descartar `fix_case` como explicación de estas pausas.

## Cambios de esta entrega

- `gpu/shadps4/core/libraries/kernel/equeue.{cpp,h}`: **BB_EQUEUE_TRACE=1**, junto
  con BB_WAIT_THREAD_TRACE, activa cronología sólo para las colas usadas por los
  hilos seleccionados. Registra `hrtimer-add`, `wait-begin`, `trigger`,
  `delivered`/`timeout`, con timestamp común a File/Cond trace, thread/tid,
  handle, nombre, ident, filter y caller inmediato. El callback se registra
  también aunque lo ejecute otro hilo. No fuerza flush ni cambia deadlines.
- `duration_us` significa: intervalo pedido en `hrtimer-add`; timeout pedido
  en `wait-begin` (-1: infinito); edad desde AddEvent en `trigger`; tiempo de
  espera en `delivered`/`timeout`. La edad de un timer periódico es desde su
  registro, no desde su último tick; para este HR OneShot es la latencia buscada.
  Trigger se imprime bajo el mutex del evento, antes de notify_one; el trazado
  optativo puede añadir su costo de impresión al tiempo de entrega. Los timers
  pequeños se entregan desde el waiter y no pasan por TriggerEvent.
- `tests/test_runtime.c`: modo `--equeue-timer-probe`, 20 timers de 3 ms a través
  del servicio real, sin cargar guest ni inicializar GPU. Verifica ident/filter
  y entrega; imprime tiempos, sin imponer un límite de scheduler artificial.
- `tests/test_gpu_runtime_stubs.cpp`: stubs de las dos funciones de diagnóstico
  del runtime para los targets de renderer sin guest.
- Este informe reemplaza la conclusión pendiente de la corrida anterior.

## Verificación

`ufc3-build --test`: **exit 0** con la revisión final; checks de runtime,
filesystem/mods, audio, semáforos, FP, AppContent, red y **42 tests Python OK**.
Log: `/tmp/ufc83-equeue-final-build.log`.

Con el binario final, `BB_EQUEUE_TRACE=1 BB_WAIT_THREAD_TRACE=runtime-test
out/runtime-test --equeue-timer-probe`: **exit 0**, 20/20 timers entregados;
mediana **3.040 ms**, máximo **3.076 ms**. Registra 20 altas, 20 comienzos,
20 disparos y 20 entregas, con filter=-15 y los ident esperados.
Log: `/tmp/ufc83-equeue-final-probe.log`. Esto descarta un redondeo fijo a 70 ms
en este probe sin carga; **no mide el servicio bajo la carga de la sesión guest**.
`git diff --check`: OK. No se ejecutó el juego.

## Una sola prueba manual pendiente

Mismos ajustes, sin borrar temp0. Desde este worktree:

```bash
BB_SAVE_LOG=1 BB_FILE_TRACE=2 BB_EQUEUE_TRACE=1 \
BB_WAIT_THREAD_TRACE='posix,UI Update' \
BB_WAIT_LOG="$PWD/logs/ufc83-equeue.waits" bash ./run.sh
```

Menú hasta ver imágenes → Fight Modes → volver y esperar → Fight Now hasta ver
retratos → volver → salir. Enviar el log automático, frames.csv y
`ufc83-equeue.waits`. No se pide A/B ni otro sampler. Correlacionar cada
`hrtimer-add` de posix con `trigger` y `delivered` de igual cola/ident:
Un trigger a ~70 ms para un pedido de 3 ms aislaría retraso del servicio;
trigger a ~3 ms y entrega tardía apuntaría al despertar/scheduler. Múltiples
pares de ~3 ms antes de la siguiente GNF demostrarían polling repetido y
dejarían pendiente al productor del estado. Esta prueba **diagnostica**; no se promete reducción de los 3 s.

## temp0, retratos y UFC-81

La revisión previa de runtime_file_configure/run.sh/launcher no encontró limpieza
de temp0; la ausencia de cap_* en B no identifica quién los quitó. Las GNF del
menú son independientes de la captura SPG; la demora se reproduce al volver al
menú sin reiniciar. No se modificó `25ff544`, BB_READBACK_LINEAR_ONLY ni la captura
de Career/post-pelea. Fight Now aún requiere correlacionar su propia espera;
no se extrapola esta pila del menú a todos sus retratos.

**UFC-81 sigue siendo útil como optimización de picos de upload/protección en
Deck; el A/B en PC no mejoró la latencia y no resuelve issue #44.**
