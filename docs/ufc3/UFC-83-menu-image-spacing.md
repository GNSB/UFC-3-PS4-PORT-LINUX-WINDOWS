# UFC-83 / issue #44: corrida de las 14:05 (tercera revisión)

**Estado: causa final pendiente; no hay arreglo acreditado de la latencia.** La nueva
corrida demuestra que seguir únicamente al despachador no explica los huecos: hay
ráfagas de trabajo enviadas por UI Update, seguidas de procesamiento lento en posix.
No se ejecutó el juego ni se cambiaron timers, señales, readbacks, persistencia o las
protecciones de Career/post-pelea. Esta entrega agrega diagnóstico, no una mejora de
velocidad que deba darse por validada.

## Cadena observada, con tiempos y sitios

Fuente: `logs/20261020_140521.log`, `logs/ufc83-next.waits`,
`logs/20261020_140521.frames.csv`. Tiempos del File/Cond/Wait trace, en segundos
respecto de configure del filesystem; no son tiempos del video anterior.

- Productor: **UI Update, tid 129581**. Broadcast sobre `cond=0x3a583ab0`, sitios
  `+0x1a3933a < +0x1daa137 < +0x1daa35f`.
- Consumidor: **posix, tid 129584**. Espera indefinida, `deadline_rt_ns=0`, sitios
  `+0x1a391ff < +0x1da9eab < +0x1da9df1`; resultado host 0 al despertar.
- Lector del BIG: **/app0, tid 129484**. Por ejemplo, tras pedir `tile_Ad_generic`
  a 46.517276, lee 147429 bytes de `inxp_ui_40.big` a 46.517415, en 0.03 ms.

El texto ejecutable del ELF 1.14 (`out/eboot.elf`, PT_LOAD ejecutable, offset 0x4000,
VA 0, longitud 37652556) confirma el significado de los sitios:
`+0x1daa050` añade trabajo a la cola; almacena el puntero en `+0x1daa116`, llama
al wrapper del broadcast en `+0x1daa132` y retorna en `+0x1daa137`.
`+0x1daa35a` llama a esa función y retorna en `+0x1daa35f`.
El consumidor prueba extraer trabajo en `+0x1da9e86 -> +0x1daa150`; sólo si recibe
NULL llama al wait en `+0x1da9ea6` (EDX=-1); retorna en `+0x1da9eab` y reconsulta.
Los sitios son de una cola genérica: el trace no incluye la identidad de cada tarea;
no permite asignar cada broadcast a una GNF concreta.

| Evento | Primer ingreso | Regreso posterior |
| --- | ---: | ---: |
| Ad / pages_fill / ras_rl | 46.517276 / 46.613551 / 46.704512 | 69.084719 / 69.147014 / 69.242593 |
| posix vuelve a esperar cola | 47.121333 | 69.653987 |
| UI Update siguiente ráfaga | 33 broadcasts, 47.668746–47.679168 | 33 broadcasts, 70.199141–70.208540 |
| posix termina ese wait | 47.668770 | 70.199162 |
| tile_fight_modes | 47.668798 | 70.199233 |

Al primer ingreso, UI Update emite 27 broadcasts entre 46.216204 y 46.234923;
la primera condición termina a 46.216233 y la primera GNF llega a 46.216268.
Al regreso también envía una ráfaga (17 broadcasts entre 68.872241 y 68.894754,
además de los envíos inmediatamente anteriores a 68.869).

**Dos intervalos distintos:**

1. De 46.216233 a 47.121333 (905 ms), posix no vuelve a esperar la condición,
   ni registra otro wait instrumentado. UI Update ya envió 27 broadcasts en
   18.719 ms. Lo mismo sucede entre 68.895 y 69.654, y durante el lote de Fight
   Modes enviado a 47.669–47.679: las GNF siguen llegando espaciadas cuando ya no
   hay nuevos broadcasts del productor. Además, posix hace sus propios broadcasts
   justo antes de abrir GNF (p.ej. 46.517241), compatibles con tareas que encolan
   más trabajo. No corresponde afirmar «UI Update manda una textura cada 70 ms».
2. De 47.121333 a 47.668770 sí espera una cola sin trabajo elegible, unos 547 ms;
   de 69.653987 a 70.199162 sucede lo mismo, unos 545 ms. Tras el broadcast,
   retorna en **24 y 21 microsegundos**, respectivamente. La primera apertura está
   a 52 y 92 microsegundos del broadcast. No son señales perdidas ni un timeout
   de 70 ms redondeado. La espera indefinida no tiene deadline que redondear.

## Qué queda sin identificar

La ventana de muestras que termina después del lote inicial (línea 9701) atribuye
57.6% de posix a `libc.so.6+0xa0950 (from +0)`, 34.1% al mismo RIP con candidato
`+0x1a391ff` y 7.9% con candidato `+0x2a9e558`. La última ventana (línea 10370)
da 53.6%, 43.4% y 2.8%. Son agregados de ventana, no muestras con timestamp que
puedan asignarse a cada hueco. El sampler anterior sólo conserva RIP y el primer
valor dentro del rango de imagen encontrado en 2048 bytes de stack.

En la libc de esta máquina, `objdump -d --start-address=0xa0920
--stop-address=0xa0980 /usr/lib/libc.so.6` muestra **una instrucción syscall en
+0xa0950, ret en +0xa0952**, dentro del wrapper interno de syscall cancelable.
Eso no identifica la syscall: hacen falta registros y pila host. **No permite
llamarla futex, sleep, espera de GPU o lectura lenta.** Tampoco debe interpretarse
`from +0` como código guest +0. `+0x2a9e558` ni siquiera pertenece al PT_LOAD
ejecutable del eboot: en boot.bin contiene datos. Es un candidato encontrado por
barrido de stack, no evidencia de un llamador. Esta limitación también afecta a
sitios como `+0x297de30` en el agregado de UI Update.

El `.waits` sigue saturado (512 claves; el log ya avisa omisiones). El trace detallado
sí evita esa tabla para posix, pero sólo cubre las primitivas instrumentadas; no cubre
cada bloqueo interno host ni rwlocks. UI Update registra principalmente condición
(aprox. 0.25 llamadas/frame, 8 ms acumulados/frame), sin cronología ni objeto en
esta corrida. No se puede atribuir su pausa de 545 ms a timer, flip o GPU con eso.
AssetStream::Lo/Tr/Un tiene esperas de semáforo agregadas, sin asociación de tarea o
GNF; no identifica al emisor de esta cola. El emisor lo acredita Cond trace.

En el lote inicial los diez reads del BIG cuestan como máximo 0.03 ms, y en el
lote siguiente 0.04 ms. Los CSV de readbacks están vacíos; los contadores registran
cero read faults y los EOP no quedan pendientes. Los frames siguen a ~8.33 ms.
Eso debilita las hipótesis de disco/readback/flip, pero no identifica qué bloqueo
host ocupa a posix. No hay base para afirmar que una PS4 tarda lo mismo o que el
juego despacha una textura por tick, ni para tocar sus deadlines o prioridades.

## Cambios de esta revisión

- `src/runtime_kernel.c`: `BB_SAMPLE_LOG` con `BB_SAMPLE_THREAD` guarda muestras
  temporales cada 16 señales (~9 ms). Conserva RIP, RAX/RDI/RSI/RDX/R10, cadena
  RBP (hasta 16 frames) y 16 palabras de stack etiquetadas como **candidatos**.
  Simboliza host con módulo/offset fuera del handler. Un ancla relaciona el reloj
  monotónico con File/Cond trace. El buffer SPSC es acotado y cuenta pérdidas;
  no escribe ni toma locks de stdio desde SIGPROF. Preserva errno al interrumpir.
  Una biblioteca sin frame pointers puede omitir frames: la cadena no es completa.
- `src/runtime_kernel.c`, `src/runtime_mutex.c`, `src/runtime.h`:
  `BB_WAIT_THREAD_TRACE` acepta nombres exactos separados por coma. Así se pueden
  registrar tanto consumidor como productor en una sola corrida, con objetos,
  deadlines y emisores de las condiciones donde espera UI Update.
- `tests/test_runtime.c`: smoke optativo `--sample-trace-smoke`, sin guest/GPU;
  comprueba salida temporal y conservación de errno bajo señales.

No se cambiaron semánticas de carga. Los diagnósticos nuevos son optativos y
pueden perturbar el timing. `gpu/third_party/fsr-vulkan` ya estaba modificado al
empezar; no forma parte de esta entrega.

## Única corrida manual solicitada

Mantener ajustes gráficos y temp0; ejecutar desde este worktree:

```bash
BB_SAVE_LOG=1 BB_FILE_TRACE=2 BB_WAIT_THREAD_TRACE='posix,UI Update' \
BB_SAMPLE_THREAD=posix BB_SAMPLE_LOG="$PWD/logs/ufc83-host.samples" \
BB_WAIT_LOG="$PWD/logs/ufc83-host.waits" bash ./run.sh
```

Menú hasta ver imágenes → Fight Modes → volver y esperar → Fight Now hasta
retratos → volver → salir normalmente. Enviar el log automático, frames.csv,
readbacks.csv y los dos archivos `ufc83-host.*`. No hay otra combinación A/B.

La prueba debe revelar la pila host dentro de los huecos (y registros si sigue en
syscall) y el wait con objeto/emisor de UI Update durante la pausa de ~545 ms.
Si falta algún frame host, quedan módulo/offset y candidatos para resolverlo;
no es garantía de que todos los llamadores puedan reconstruirse. Hasta conocer
esa dependencia, la causa final y un arreglo de la latencia siguen pendientes.

## Verificación

`ufc3-build --test`: **exit 0**, binario generado, checks de runtime, filesystem,
audio, semáforos, event flags, FP, AppContent y red; **42 tests Python OK**.
Log local: `/tmp/ufc83-build-final-review.log`.
`out/runtime-test --sample-trace-smoke`: **exit 0**, salida temporal y errno PASS;
log `/tmp/ufc83-sample-smoke-final.log`. `git diff --check`: OK.
No se ejecutó el juego. Estas comprobaciones validan el diagnóstico añadido, no una
mejora visual ni la causa final.

## temp0 y UFC-81

`runtime_file_configure` sólo crea/monta directorios, sin vaciarlos. El borrado
relevante de run.sh es el cache CUSA06534, no temp0; no se encontró una limpieza
createplayer/images en el launcher. La ausencia de cap_* en B no prueba quién
los quitó ni que el juego los regenere obligatoriamente en todas las sesiones.
Estas GNF del menú no son
SPG capturados; la demora también ocurre al regresar sin reiniciar. No corresponde
cambiar temp0 o readback para solucionar esta cadena aún no resuelta.
**UFC-81 sigue valiendo como optimización de picos de upload/protección en Deck;
el A/B en PC no mejoró la latencia y no resuelve issue #44.**
