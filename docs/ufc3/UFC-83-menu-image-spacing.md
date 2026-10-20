# UFC-83 / issue #44: espaciado de pedidos del menú (segunda revisión)

Estado: investigación incompleta; esta entrega corrige el diagnóstico, no acredita un
arreglo de la demora. No se ejecutó el juego. No se modificaron GPU/readbacks, capturas,
archivos de usuario ni protecciones de Career/post-pelea.

## Evidencia reproducible

Fuentes locales: `~/ufcCOntext/ufc44-runs/ufc83.{log,csv,waits}` y los dos ensayos anteriores.
No hace falta inferir un problema de disco: en el log, `tile_Ad_generic` se pide a 41.273 s,
`pages_fill` a 41.364, `ras_rl` a 41.461 y `indicator_home` a 41.527. Después,
`tile_fight_modes` llega a 42.473, `tile_fight_modes_bg_over` a 42.559,
`tile_fight_modes_over` a 42.666 y `tile_skill_challenges` a 42.726.
Los reads correspondientes de `inxp_ui_40.big` terminan en 0.01–1.61 ms.
Los pedidos siguen espaciados incluso para texturas de 1280 o 4352 bytes.
Son GNF empaquetados, no capturas SPG de peleadores creados.

El intervalo contiene 120 FPS, mediana 8.33 ms, p99 8.37 ms, cero frames sobre
1.5 veces la mediana; el hilo GPU está 87.7% ocioso. EOP: 44131 labels escritos,
0 pendientes. Esto no demuestra la causa del espaciado, pero no acompaña la hipótesis
de una GPU ocupada durante cada pausa de 60–100 ms.

`ufc83.waits` identifica `posix`, tid 117448. Su condición tiene los sitios
`+0x1a391ff < +0x1da9eab < +0x1da9df1`. Desensamblación estática del texto del
ELF 1.14 (segmento ejecutable, offset de archivo 0x4000, VA 0):

- `+0x1da9de0`: obtiene la cola y llama a `+0x1da9e30`.
- `+0x1da9e80`: intenta extraer trabajo mediante `+0x1daa150`.
- Si devuelve NULL, `+0x1da9e9b` carga EDX=-1, `+0x1da9ea6` llama a
  `+0x1a39310`; retorna en `+0x1da9eab` y vuelve a consultar la cola.
- El wrapper llega a la llamada sin timeout en `+0x1a391fa`, que retorna en
  `+0x1a391ff`: `pthread_cond_wait`. La rama con deadline termina en otro sitio,
  `+0x1a391e7`.

Por lo tanto **la espera registrada es por trabajo en una cola vacía, indefinida**,
no un sleep de 70 ms ni un timeout redondeado. No alcanza para demostrar que cada
hueco entre GNF corresponda a esa espera.

## Por qué el `.waits` no permite cerrar la causa

La tabla de `runtime_wait_note` tiene 512 slots, nunca libera las claves y descartaba
silenciosamente sitios nuevos al llenarse. El archivo tiene **exactamente 512 claves
distintas** (identidad de hilo, tipo y tres sitios, quitando las dos columnas numéricas).
Por eso no se puede descartar un sleep o un semáforo del mismo hilo por su ausencia.
Además, cada fila es un agregado por ventana de 600 frames; `0.00` es cantidad/frame
redondeada a dos decimales, no cero llamadas. Una espera larga se contabiliza al volver.
No hay timestamp por llamada ni identidad de la condición ni del emisor de la señal.

Los `AssetStream::*` tienen esperas sema (por ejemplo `Tr` en `+0x41f6fc0` y
`+0x4266fc0`, `Lo` en `+0x40c6fc0`) y `Lo` una condición en `+0x40c6dd0`.
Con la misma tabla saturada, no se puede asociar estas esperas a una GNF concreta.
Las direcciones mayores que el texto del eboot tampoco deben etiquetarse como funciones
del eboot sin resolver el módulo o código que el juego cargó allí.

Los 15.3 waits / 40.50 ms de EAThread Timer Queue son sumas por frame de la cola;
no equivalen a una espera individual de 40.50 ms del hilo posix. FlipManager usa su
propio timeout de 5000 us. El log no establece dependencia de posix con ninguna de
esas colas. No hay base para cambiar su semántica ni afirmar que PS4 carga un asset
por tick. El port usa pthread_cond_wait nativo en Linux para esta condición.

## Cambios

- `src/runtime_kernel.c`: avisa cuántas esperas se pierden al saturarse los 512 slots.
  `BB_WAIT_THREAD_TRACE=posix` registra cada espera instrumentada del hilo seleccionado
  (cond/mutex/sema/sleep), con timestamp de retorno, duración y tres sitios, antes de
  consultar la tabla acotada. Sigue siendo un trace de las primitivas instrumentadas,
  no de todas las formas posibles de bloqueo.
- `src/runtime_mutex.c`: con esa misma variable registra begin/end de condiciones del
  hilo seleccionado y signal/broadcast de cualquier hilo sobre esas condiciones.
  Incluye identidad del objeto host, deadline absoluto realtime (0 = indefinido),
  resultado host y sitios. Los objetos quedan marcados hasta destruirse; las señales
  anteriores a su primera espera seleccionada no quedan registradas.
- `src/runtime_file.c` y `src/runtime.h`: timestamp común desde configure del filesystem,
  seis decimales y tid en File trace / Cond trace / Wait trace.

Sin variable nueva no se emiten traces detallados. No se cambiaron deadlines, señales,
política de readback ni lifetime de imágenes. La salida de diagnóstico puede perturbar
ligeramente el timing; las muestras SIGPROF ya existentes permiten identificar si el hilo
trabaja dentro del guest entre llamadas, sin loggear cada instrucción.

## temp0 y UFC-81

`runtime_file_configure` sólo crea/monta directorios; mkdir sobre una carpeta existente
no la vacía. El único borrado relevante de `run.sh` es el cache de CUSA06534, no temp0.
No encontré limpieza de createplayer/images en el launcher. La ausencia de cap_* al
inicio de B no identifica quién los quitó: no demuestra ni limpieza del runtime ni
regeneración obligatoria del juego en cada sesión. El menú también demora al regresar
sin reiniciar, y sus GNF no pasan por SPG. No hace falta cambiar persistencia para
investigar este intervalo.

`25ff544` filtra render targets lineales (por defecto storage/tiny; `ONLY=a` restaura
la selección anterior). No hay evidencia aquí para revertirlo o forzar `ONLY=a`:
ese cambio requiere validar las capturas, y no explica por sí solo pedidos GNF tardíos.

**UFC-81 sigue siendo una optimización candidata para los picos de upload/protección
observados en Deck, pero el A/B del usuario no mejoró la latencia de imágenes en PC;
no resuelve el espaciado de issue #44.**

## Una sola prueba manual pendiente

Desde este worktree, mantener los ajustes gráficos habituales y ejecutar una vez:

```bash
BB_SAVE_LOG=1 BB_FILE_TRACE=2 BB_WAIT_THREAD_TRACE=posix BB_SAMPLE_THREAD=posix \
BB_WAIT_LOG="$PWD/logs/ufc83-next.waits" bash ./run.sh
```

Entrar al menú principal y esperar las imágenes; abrir Fight Modes, regresar y esperar;
entrar a Fight Now hasta ver retratos y volver; salir normalmente. Enviar el `.log`,
`.frames.csv`, `.readbacks.csv` de esa ejecución y `logs/ufc83-next.waits`.
No borrar temp0 ni cambiar variables de readback. No hace falta otra combinación A/B.

Interpretación: begin→signal largo identifica al despachador tardío mediante su hilo y
sitios; signal→end largo incluye scheduling y readquisición del mutex; end→pedido largo
con muestras de posix identifica trabajo del guest o host. Wait trace revela sleeps o
semáforos omitidos por el agregado; Cond trace distingue deadline y señal. Con eso falta
resolver el llamador que impone el ritmo y recién entonces aplicar un arreglo causal.

## Verificación de esta entrega

`ufc3-build --test`: exit 0, binario generado y checks de runtime, audio, filesystem,
semáforos, FP, AppContent, event flags y red; 42 tests Python OK.
Log local: `/tmp/ufc83-build-final.log`. Smoke del trace, sin juego:
`BB_WAIT_THREAD_TRACE=runtime-test out/runtime-test`: exit 0, registros Wait trace con
tid/duración/sitios y contratos PASS (ese test no configura filesystem, por eso su epoch
es el origen monotónico del host). No valida la relación temporal del guest: falta la
única corrida manual indicada arriba.

El estado modificado de `gpu/third_party/fsr-vulkan` ya estaba presente al empezar y no
forma parte de esta entrega. No se hizo commit, merge, rebase, push ni cambio de rama.
