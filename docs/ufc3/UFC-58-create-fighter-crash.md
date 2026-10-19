# UFC-58 — creación de peleador, null write en 0x402882

Estado: diagnóstico parcial; **no hay un arreglo funcional confirmado**. No se ejecutó
el juego. La causa inmediata es la falta de un nodo en un pool guest de cuatro
resultados de cambios de aspecto. Falta determinar por qué el consumidor no libera
resultados a tiempo; el log disponible no permite atribuirlo a una API del runtime.

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

Compilación mediante `ufc3-build`: **OK**, salida 0, genera `out/sacredfire`
y versión `ufc3-v0.1.6-dirty`. Hay warnings de CMake y ODR en FSR de terceros.
`git diff --check`: OK. El build aplica sus patches habituales al submódulo
`gpu/third_party/fsr-vulkan`; no se editaron esos patches para este ticket.
Esa orden compila Linux: no valida el bloque `_WIN32` del diagnóstico.
No se ejecutó el juego ni se cambió la rama, el índice o la historia git.
Quedan pendientes la compilación Windows y la reproducción manual anterior.
