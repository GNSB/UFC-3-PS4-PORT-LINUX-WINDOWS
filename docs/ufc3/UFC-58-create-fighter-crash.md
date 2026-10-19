# UFC-58 — creación de peleador, null write en 0x402882

Estado, tercera vuelta (2026-10-19): **arreglo en el runtime** (`src/runtime_appearance.c`):
contrapresión entre quienes toman/publican resultados de aspecto y quien los retira, enganchada
en el lock del pool y verificada por bytes. Sin probar en el juego todavía. Las secciones de la
primera y segunda vuelta quedan abajo como estaban; la tercera está al final.

## Evidencia y alcance

- Reporte #26: `~/ufcCOntext/github-issues/i26-20261018_203052.log`, versión
  `ufc3-v0.1.6`, Windows, CUSA06534 01.14, GTX 1070 Ti / driver 551.86.
- Se analizó, en lectura, `/home/myzk/Projects/ufc3-native/out/boot-linked.bin`.
  SHA256: `cea8cdf4e01b63ea21e80758c73d0ac1ced53d5e35e5f96dd552ccc204a0f230`.
  No se dispone del binario exacto del usuario para comparar hashes. Las direcciones
  y el estado de registros del log son coherentes con este desensamblado.
- #20 no tiene log: no se puede afirmar que sea el mismo fallo, ni deducir una causa
  ligada a VRAM por el hardware de ese reporte.

## Qué puntero es nulo

La función empieza en `0x402710`. Llega desde `0x4021c0` mediante el `call`
de `0x40267c`, cuyo retorno es el primer frame `0x402681` del log.
`0x405c18` llama virtualmente al método `+0x30`, con retorno `0x405c1b`:
la tabla contiene `0x4021c0` en `0x2424fe8`. Los siguientes frames son
`0x4073c3`, `0x69110`, `0x74129`, `0x74e16`, hasta el hilo RenderThread.

En `0x402710`, `r13` conserva el objeto recibido en `rdi`. Este bloque no llama
a una función HLE para conseguir el puntero:

```asm
402836  mov rsi, [r13+0x178]       ; secuencia anterior
40283d  lea rax, [rsi+1]
402841  mov [r13+0x178], rax
402848  mov r8, [r13+0x1a0]       ; lista de nodos libres
40284f  test r8, r8
402852  je 0x40285c
402854  mov rax, [r8]
402857  mov [r9], rax             ; retira nodo libre
40285a  jmp 0x402882
40285c  mov r8, [r13+0x1a8]       ; siguiente nodo sin usar
402863  cmp r8, [r13+0x1b0]       ; final del pool
40286a  je 0x40287f
40286c  mov rax, r8
40286f  add rax, [r13+0x1b8]      ; stride
402876  mov [r13+0x1a8], rax
40287d  jmp 0x402882
40287f  xor r8d, r8d              ; pool agotado -> NULL explícito
402882  mov [r8], r10             ; fallo, bytes 4d 89 10
```

Por tanto, el destino nulo es **r8**, no rbx. La ruta normal que lo produce
es lista libre vacía y cursor igual al final. El log conserva `rsi=0xc4` y
`rax=0xc5`, coherentes con la secuencia 196 -> 197 que queda en rax al tomar
esa ruta. No significa que haya 196 nodos pendientes. El log no imprime r8,
r13 ni el contenido del objeto, por lo que aún hay que descartar corrupción
del pool o un fallo de sincronización.

El constructor configura el pool en `0x4015a1..0x4015df` con tamaño `0xe0`,
stride `0x38` y alineación 8, mediante `0x42e30`: **cuatro nodos**.
`[obj+0x188/0x190]` es la lista activa y `[obj+0x198]` su cuenta.
El flag del nodo `+0x30` se inicia a cero en `0x40299f`.

## Cómo se reciclan los resultados

- Antes de asignar, `0x4027c0..0x402834` recorre la lista activa. Recicla
  los nodos con flag `+0x30 == 0`, devuelve su slot de datos al freelist
  de índices y devuelve el nodo al pool. Conserva los que tienen flag 1.
- `0x403210` toma el resultado más reciente si su secuencia es mayor que
  `[obj+0x180]`; devuelve secuencia, datos y cuenta, marca `node+0x30=1`
  en `0x403269` y actualiza `[obj+0x180]`. Está en la misma vtable,
  método `+0x38` (`0x2424ff0`).
- `0x4032a0`, método `+0x40` (`0x2424ff8`), busca por secuencia, procesa
  sus datos y retira los resultados anteriores al nodo siguiente al elegido
  (`0x40342b..0x4034c7`). Esa retirada devuelve slots y nodos al pool.
- Estas funciones usan el lock guest de `obj+0xd0` a través de `0x443f0`
  y `0x44580`. El bloqueo acaba en imports de mutex; no es en sí un fence
  Vulkan. El procesamiento observado manipula vectores y matrices de CPU.

Hipótesis prioritaria: se toman resultados de deformación/aspecto, pero su
procesamiento/retiro se demora o se omite; cuatro nodos quedan retenidos con
flag 1 y el productor agota el pool. También debe descartarse que el lock
falle o que se corrompan los enlaces. No hay evidencia suficiente para cambiar
gnmdriver, la asignación de memoria o el código de BC/storage aliases de UFC-6.

## Qué dice el log antes del fallo

No aparecen errores de asignación directa, de bind de arenas, ni avisos de imports
sin implementar. Las asignaciones directas de inicio y los binds observados
terminan normalmente; el último es `Arena bind 50 ... result Success`.
La ausencia de errores no prueba que toda asignación haya tenido éxito.

Hay expansiones de vistas sRGB/UNORM `[T6a]`, draws de piel `[T7]`, un aviso
`Unimplemented depth overlap copy` y estadísticas de readbacks. Ninguno
identifica el nodo guest ni demuestra que cause su retención. Se repite
`open(/temp0/createplayer/cap_20000) -> not found`; podría ser una consulta
de caché, y falta trazar su llamador para establecer relevancia. Los fallos de
montaje de saves y la ausencia inicial de `profile.bin` tampoco vinculan el
pool a una asignación GPU fallida.

## Cambio de diagnóstico

`src/platform/platform_win.c` imprime ahora r8..r15 en el informe fatal.
Para RIP guest `0x402882`, r8 nulo y opcode comprobado `4d 89 10`, agrega:

- `UFC-58 appearance pool`: objeto r13, secuencia publicada, última tomada,
  cuenta activa, lista libre, cursor/final y stride.
- Hasta cuatro nodos activos: dirección, enlaces, secuencia, slot y flag `taken`.

Las lecturas usan `platform_safe_read` / `ReadProcessMemory`, incluyen control
de overflow del objeto y tienen un límite de cuatro nodos, incluso si hay ciclos.
El snapshot no toma locks: otros hilos pueden seguir modificando la lista durante
la captura. Se captura sólo al terminar por fallo; no modifica el estado guest ni intenta
continuar tras el crash. No cambia defaults del launcher ni write windows.

## Prueba manual concreta y siguiente decisión

En Windows, con una compilación que incluya este diagnóstico:

1. Abrir Career > crear peleador > aspecto. Anotar preset y slider iniciales.
2. Mover lentamente un slider varias veces; después alternar sus extremos
   rápidamente. Registrar si el modelo sigue aplicando cada cambio.
3. Cambiar varios presets, volver a sliders y repetir. Ejecutar una sola sesión
   a la vez. Conservar el log completo y la secuencia exacta si crashea.
4. En `UFC-58 appearance pool`, comprobar `free=0`, `cursor=end`, `stride=56`
   y cuenta activa 4. Cuatro nodos `taken=1` apoyan la hipótesis de retención.
   Valores incoherentes o enlaces inválidos orientan a corrupción/sincronización.
5. Si se confirma retención, trazar por objeto/secuencia los métodos `0x403210`
   y `0x4032a0` (breakpoints en un depurador, incluyendo sus retornos y hilos).
   Determinar qué llamador deja de retirar resultados antes de cambiar el runtime.
   El snapshot final por sí solo no demuestra qué servicio HLE sería responsable.

Criterio para validar un futuro arreglo: sliders y presets aplican cambios visibles,
sin este fault durante la misma secuencia que lo reproduce; después salir y volver
a crear/editar el peleador. No aumentar el pool ni saltar la escritura nula como
supuesto arreglo: ocultaría el atasco y podría perder cambios o exceder los buffers
de datos asociados, que también tienen cuatro slots.

## Validación de esta entrega

Esta sección corresponde a la primera vuelta (`3fd4d33`). La segunda está al final.

Compilación mediante `ufc3-build`: **OK**, salida 0, genera `out/sacredfire`
y versión `ufc3-v0.1.6-dirty`. Hay warnings de CMake y ODR en FSR de terceros.
`git diff --check`: OK. El build aplica sus patches habituales al submódulo
`gpu/third_party/fsr-vulkan`; no se editaron esos patches para este ticket.
Esa orden compila Linux: no valida el bloque `_WIN32` del diagnóstico.
No se ejecutó el juego ni se cambió la rama, el índice o la historia git.
Quedan pendientes la compilación Windows y la reproducción manual anterior.

## Segunda vuelta — seguimiento estático (2026-10-19)

Se mantuvo intacto el diagnóstico de `platform_win.c`, incluida la línea de
registros compartida con UFC-57. No se modificaron el juego, el runtime, los defaults
del launcher ni las write windows. La restricción de no improvisar ante un ticket
más ambiguo aplica aquí: los resultados siguientes no justifican elegir entre dos
arreglos de sincronización, ni introducir uno por defecto o por variable de entorno.

### Toma y retiro son operaciones distintas

La hipótesis «consumed no avanza» no se deduce del agotamiento. Con una lista y un
lock correctos, cada ejecución del productor recicla **todos** los nodos con
`taken=0` antes de asignar. Cuatro nodos retenidos con `taken=1` implican que la
toma sí se ejecutó: `0x403210` escribe tanto ese flag como `owner+0x180` bajo el
mismo lock. Lo pendiente sería el retiro posterior, no necesariamente la toma.
El snapshot fatal de la primera vuelta sigue siendo necesario para distinguir
esto de corrupción de enlaces o del estado del pool.

### Llamador que toma el resultado

`0x438270` invoca el método `+0x38` de `[r12+0x27e0]` en `0x4382ba`, con
tres salidas de stack (secuencia de 64 bits, puntero, cuenta de 32 bits), y prueba
`al` en `0x4382bd`. La firma y el slot coinciden con `0x403210`.
Si hay resultado:

1. Pasa datos/cuenta al método `+0x20` de `[r12+0x2758]` (`0x4382d5`).
2. Guarda la secuencia en `[[r12+0x18]+0x168]` (`0x4382e0`).
3. Procesa los elementos mediante `0x1204560` (`0x43830f`).

Es un callback CPU: el puntero `0x438270` está en `0x25c1d18`.
`0x437fc0` y `0x438160` lo copian a una tarea construida con `0x13d9340`,
y la encolan mediante `0x464ea0`, con argumento 2 o 1, respectivamente.
`0x464ea0` sólo agrega la tarea a una colección bajo el mutex guest `queue+0x88`;
no espera un fence ni señala un semáforo en ese bloque. `0x465160` drena las
colecciones y llama `0x4655c0`; éste despacha el callback de una tarea nativa por
su puntero `+0x18` (`0x46563a`). **No se estableció el nombre del hilo que drena
esa cola**: llamarlo RenderThread o un worker concreto sin seguir su despacho
sería una suposición. El log sólo identifica RenderThread para el productor.
La identidad de las instancias entre ambos lados sigue requiriendo una captura.

### Llamador que devuelve los nodos

`0x409740` carga el pool desde `component+0xb08`, toma la secuencia desde
`[[component+0x30]+0x168]` y llama al método `+0x40` en `0x409773`, pasando
además `component+0xa50` como destino de matrices. Es la firma de `0x4032a0`;
el productor usa el mismo campo `component+0xb08` en `0x405bee..0x405c18`.
La devolución efectiva al freelist sigue siendo `0x403470..0x4034c7`.

`0x409740` está en la vtable `0x2425030`, slot `+0x118` (`0x2425148`).
Su llamador de actualización está en `0x121cbe0`, llamada virtual `0x121cc17`:

```text
0x409280 (vtable +0x38)
  si component+0xbd4 != 0: 0x121cbe0
    0x12084a0: actualización base
    0x698f0(component+0x20, component+0x3c)
    si component+0x30 != NULL y la consulta devuelve true:
      vtable +0x118 -> 0x409740
        si component+0xb08 != NULL y component+0xbd4 != 0:
          vtable +0x40 -> 0x4032a0(secuencia del contexto +0x168)
```

`component+0xbd4` se configura en `0x404d5f..0x404d7d` desde la propiedad
**`skinning_method`** (string `0x1e7779f`), no desde un contador de GPU observado
en esta función. `0x698f0` consulta el gestor guest global `0x29828f0` por los
métodos `+0x50`, `+0x38` y después `+0xe0` del objeto devuelto
(`0x69935`, `0x69948`, `0x69958`); si no hay resultado de la primera consulta,
devuelve false. **Falta resolver ese despacho `+0xe0` y el hilo de actualización**.
No se encontró aún en esa cadena una lectura de label, EOP, flip, SubmitDone o
una espera de kernel. Esto no demuestra que no exista una dependencia indirecta.

### Auditoría del runtime y límites de la evidencia

- `liverpool.cpp`: EOP se ejecuta mediante `RunInOrder(RunEventWriteEop)`.
  Un retorno false de `RunInOrder` significa que la tarea **ya se ejecutó** de
  forma síncrona (`vk_rasterizer.cpp`), no que el evento se perdió. EOS sigue
  el mismo contrato cuando hay rasterizer.
- `HonestLabels()` difiere EOP/EOS hasta el tick Vulkan mediante `SignalAfterGpu`.
  En stage B primero quedan en `local_signals`; `PublishSignals` los transfiere
  a `gpu_signals`. `GpuSignalLoop` espera el timeline y ejecuta cada callback
  antes de quitarlo de la cola. En el camino sin HonestLabels, EOP espera las
  copias o usa `SignalAfterHostCopies`; el scheduler cuenta señales emitidas y
  terminadas, y `WaitDeferredSignals` fuerza su grabación antes de esperar.
- `PendingFenceValue` sólo adelanta la evaluación de `WAIT_REG_MEM` del decoder.
  **No escribe el valor en memoria CPU ni actualiza `owner+0x180`**. Su presencia
  no demuestra que el consumidor CPU haya visto un label correcto.
- `SignalEop` y `WriteEosLabel` escriben con `TryWriteBacking` o `StoreLabel` y
  notifican `NoteLateCommandWrite`. `runtime_memory_write_backing` escribe la
  vista del backing con stores release; esa función no invalida por sí sola una
  copia GPU. La propagación posterior depende del buffer cache. Sin una dirección
  de label vinculada al consumidor no se puede concluir si un readback posterior
  lo sobrescribe ni atribuirlo a las write windows.
- Hay rutas que merecen revisión si se demuestra esa dependencia: `GuardLabel`
  puede descartar una escritura cuando cree que el slot fue reconstruido; y
  `RELEASE_MEM` usa su propio `SignalFence`, con stores directos, a diferencia
  de los callbacks de EOP/EOS. El log #26 no muestra `Late label guarded`, ni
  aporta direcciones/valores de fences para vincular alguna de ellas al pool.
- Los semáforos y event flags de las APIs guest pasan por `src/runtime_sema.c`
  y `src/runtime_eventflag.c`. La cola CPU seguida aquí usa mutex y colecciones;
  no se identificó una llamada a esas esperas en el tramo trazado. No se cambió
  su semántica sin conocer una espera concreta que esté fallando.

No se considera esta lectura una prueba funcional de sincronización. Tampoco se
puede trasladar al crash de #26 cualquier defecto potencial que aparezca en una
ruta de GPU sin establecer que el consumidor de este pool la usa.

### Coordinación con UFC-57 y siguiente evidencia necesaria

La causa común con UFC-57 **no está demostrada**. Pool agotado es un síntoma
compartido; en UFC-58 la toma y el retiro son distintos y la condición encontrada
incluye estado CPU y `skinning_method`. No se alteró `fatal_exception`.

Para continuar sin cambiar el juego: resolver el despacho guest de `0x698f0`
(`+0xe0`) y el origen de la actualización `0x409280`; obtener en la reproducción
los hilos, objetos y secuencias al pasar por `0x4382ba`, `0x4382e0`,
`0x121cc17`, `0x409773` y `0x4034c7`, junto con el snapshot fatal existente.
En el componente hay que registrar también `+0xbd4` y el resultado de `0x698f0`.
Si el despacho termina esperando un label/evento, recién entonces se puede
comparar su dirección, valor esperado, valor escrito y visibilidad CPU con el
runtime. Si la toma avanza pero no llega el retiro, esas capturas separan una
consulta false, un contexto/secuencia incorrectos y la ausencia de actualización.

Prueba manual del arreglo cuando exista: **Career > crear peleador > aspecto**;
alternar los extremos de varios sliders, cambiar presets varias veces, volver a
los sliders y repetir; salir y volver a crear/editar el peleador. Deben aplicarse
los cambios visibles y no aparecer el fault `0x402882`. Se ejecuta una sesión a la
vez por el usuario. No se ha corrido esa prueba en esta entrega y no se declara
el ticket arreglado.

### Validación de la segunda vuelta

`ufc3-build`: **OK, salida 0**, genera `out/sacredfire`, versión
`ufc3-v0.1.6-1-g3fd4d33`. Es la compilación Linux del runtime existente;
no valida una corrección, ya que no se modificó código. Hay warnings de políticas
CMake de terceros. `git diff --check`: OK. Log de build local:
`/tmp/ufc58-build-second.log`. El submódulo `gpu/third_party/fsr-vulkan` ya
figuraba modificado al comenzar; no se tocó manualmente. No se ejecutó el juego,
no se usaron agentes paralelos y no se cambió el índice, la rama ni la historia
git. Los cambios quedan en el worktree para el despachador.

## Tercera vuelta — quién consume el pool, en qué hilo, y el arreglo (2026-10-19)

Análisis estático sobre `out/boot-linked.bin` con las relocaciones aplicadas (imagen plana,
offsets = offsets guest). No se ejecutó el juego.

### Los tres actores del pool

| rol | código | cuándo | hilo |
|---|---|---|---|
| **publica** (toma nodo; crash si no hay) | `0x402710`, desde `0x4021c0` (vtable `+0x30`) | cuando terminan los trabajos de GPU/deformación que lanzó (`+0x2e4` llega a 0) | **RenderThread** (log #26) |
| **toma** (marca `node+0x30=1`, guarda la secuencia en `ctx+0x168`) | `0x403210` (vtable `+0x38`) desde la tarea `0x438270` | cada update del personaje: `0x437d40` → `0x437fc0` encola la tarea (prioridad 2) en la cola `obj+0x27f8`; la drena el sistema `0x444090` (vtable `0x2427008`, slot `+0x38`) → `0x465170` → `0x4655c0` | el que drena esa cola (lado sim) |
| **retira** (libera todos los nodos hasta la secuencia de `ctx+0x168`) | `0x4032a0` (vtable `+0x40`) desde `0x409740` (`+0x118`) | update del componente `0x409280(dt)` → `0x121cbe0`, sólo si `skinning_method` (`+0xbd4`) y `0x698f0` dan true | el del update del componente |

- El productor no corre "suelto": en #26 la pila es el bucle principal de RenderThread
  (`0x450b9e` → tabla de callbacks `0x4a0987` → `0x62d20` → `0x630a0` → `0x1499f0`) que **entrega los
  mensajes sim→render** de los stream groups (`0x68fc0` → callback global `[0x29828f8]+0x10`,
  `0x69110`) al componente: el handler `0x407250..` compara FourCC y el mensaje `'RPOC'`
  (`0x407381`) llama `0x405b70` → `0x4021c0` → publicar. Es decir, **cada paso de simulación
  encolado puede publicar un resultado** cuando RenderThread lo procesa.
- `0x698f0` (la condición del retiro, lo que faltaba): `[0x29828f0]` es el servicio
  **`CoreSystems:IStreamGroups`** (registro `0x49fd79`, clase `0x2506b58` creada en `0x126f150`).
  `+0x50` (`0x126f7f0`) copia el estado actual (24 bytes, `entry+0x98`) del grupo `component+0x3c`;
  si está vacío devuelve false. `+0x38` (`0x126f760`) devuelve el objeto del grupo y se le pregunta
  `+0xe0(handle=component+0x20, estado)`: **¿está esta entidad en el estado que el sim ya publicó
  para este grupo?** Los grupos se configuran desde datos (`deltaCompression`, `liveCount`,
  `replaySize`, `SimThread::GetLoopFrequency`…): es el streaming de estado del hilo de simulación
  al de render. La clase concreta del objeto de grupo viene de esa configuración (campo `+0x40`
  de cada entrada) y no se resolvió estáticamente; no hay en esa cadena labels, EOP ni esperas
  de kernel.
- El pool se agota sólo con **cuatro resultados tomados sin un retiro en medio**: el productor
  recicla todo lo no tomado antes de pedir nodo, y el retiro libera hasta la última toma. Toma
  y publicación avanzan con los pasos de simulación; el retiro va con el update del componente y
  se salta si el estado del stream no está. Cuando RenderThread procesa varios pasos sim
  atrasados de una vez (publica varias veces) mientras el lado sim sigue tomando, y el retiro no
  llega entre medio, se juntan las cuatro tomas: es **el patrón de UFC-57** (el render del port
  se queda atrás del sim; en la PS4 van parejos), con otro pool.

### Qué lo atrasa en el port, y el límite de la evidencia

El log de #26 no tiene esperas largas registradas (`Wait over`, readbacks de segundos como en
#16): no se puede afirmar con ese log qué frenó al lado que retira. Lo que sí muestra justo antes
del fault es un pico de readbacks (`Buffer downloads (5 s): 416 with payload, 29651200 bytes`, frente
a ~650 KB en los intervalos previos) en el momento del cambio. Tampoco se pudo fijar estáticamente
el hilo del update del componente (el que retira) frente al del drenaje de tareas (el que toma):
**el arreglo lo mide en ejecución** y lo escribe en el log (abajo).

### El arreglo (src/runtime_appearance.c)

Contrapresión en el runtime, sin tocar el juego (no se agranda el pool ni se salta el null):

- Publicar, tomar y retirar toman el mismo lock `this+0xd0` por el wrapper EA `0x443f0` →
  `0x44470` → `scePthreadMutexTrylock` (retorno `0x444cc`) / `scePthreadMutexLock` (`0x444a4`).
  En esas dos funciones del runtime (`runtime_mutex.c`), si la dirección de retorno es la del
  wrapper, `runtime_appearance_before_lock` busca en la pila el slot de retorno del wrapper (marco
  fijo: `[Z+0x40]` es el retorno de `0x44470`; si es `0x44420`, `[Z+0x70]` es el del llamador) y
  reconoce a `0x4027a1` (publicar), `0x403239` (tomar) o `0x4032c9` (retirar). Todos esos bytes
  (los marcos de `0x443f0`/`0x44470`, las tres llamadas, el `xor r8d,r8d; mov [r8],r10` y el
  `mov byte [rax+0x30],1` de la toma) se verifican al arrancar; si no coinciden (otro eboot), queda apagado.
- Antes del lock lee el pool (sin lock, enlaces validados dentro de los 4 nodos):
  - una **toma** que dejaría al publicador sin nodo (`would_take` y espacio ≤ 1) **espera** a que
    el retiro libere;
  - una **publicación** que no encontraría nodo (espacio 0, el caso del crash) también espera.
  Espacio = nodos no tomados (los recicla) + cabeza de la lista libre + cursor sin usar.
- Sólo espera si el retiro ya se vio en **otro hilo** (si es el mismo, la espera no podría
  terminar). Tope `BB_APPEARANCE_BACKPRESSURE_MS` (250 ms por defecto); si se agota, ese pool
  sigue sin esperar hasta que vuelva a tener espacio (no se acumulan tirones).
  `BB_APPEARANCE_BACKPRESSURE=0` lo apaga.
- Log: `Appearance back-pressure (UFC-58): on…` al arrancar; una línea por pool con **el hilo de
  publicar, tomar y retirar** (`publish on thread N RenderThread, take on …, retire on …`); la
  primera espera, cada 256, y cada una que se agote; al salir, el total en el informe del runtime.
- `tests/test_runtime.c` (`appearance_backpressure`): toma que espera hasta que otro hilo retira,
  sin espera con espacio o con el retiro en el mismo hilo, publicación que se agota a los 250 ms
  y no vuelve a esperar hasta recuperar espacio.

No se hizo rebase/merge con UFC-57: el mecanismo no es el de `runtime_ui.c` (allí la señal es el
flip; aquí el estado del pool en el lock), así que no hay código que compartir. Si se mergean
las dos ramas, los conflictos son: la línea r8–r15 de `platform_win.c` (dejar una sola; la de
UFC-58 imprime `r8 =`, la de UFC-57 `r8=`), los includes/declaraciones de `runtime.h`, las
llamadas de init en `probe.c`, los reportes en `runtime.c`, `posix_mutex_lock` en
`runtime_mutex.c` (UFC-57) junto a `mutex_lock`/`mutex_trylock` (UFC-58), y `ms_since` en
`tests/test_runtime.c`, definida en ambas (dejar una).

### Prueba

Windows o Linux, una sesión: **Career > crear peleador > aspecto**; mover sliders entre extremos
rápido y varias veces, cambiar presets varias veces, volver a los sliders y repetir; salir y volver
a crear/editar. Debe aplicar los cambios y no aparecer el fault `0x402882`. En el log buscar:

- `Appearance back-pressure (UFC-58): on` — si no aparece, el eboot no es el 1.14 verificado.
- `pool …: publish on thread …, take on …, retire on …` — responde en qué hilo corre cada rol.
- `a take/publish waited … ms for the retire` — el arreglo actuó. Si dice `ran out` y después
  crashea, el retiro no estaba llegando (p. ej. `0x698f0` false) y el informe fatal de la primera
  vuelta (`UFC-58 appearance pool` con los cuatro `taken=1`) más esa línea de hilos dicen qué falta.
- Si crashea y **no** hay línea de hilos con `retire`, el retiro nunca corrió en ese pool:
  la causa es la condición del retiro, no la velocidad, y la contrapresión no aplica.

### Validación de la tercera vuelta

`ufc3-build --test`: compila `out/sacredfire` (salida del build OK, versión
`ufc3-v0.1.6-2-gca38d27-dirty` antes del commit); `runtime-test` **PASS** incluyendo
`appearance back-pressure`. Después `file-mods-test` falla en `tests/test_file_mods.c:54`
(`rmdir` de los directorios del usuario de prueba): esa prueba sólo enlaza `test_file_mods.c` y
`platform_posix.c`, que no se tocaron; por el `set -e` no corrieron las pruebas siguientes
(sema, eventflag, fp, red). La compilación Windows no se hizo aquí (el archivo entra por el glob
`src/runtime*.c` de `gpu/CMakeLists.txt`). No se ejecutó el juego.
