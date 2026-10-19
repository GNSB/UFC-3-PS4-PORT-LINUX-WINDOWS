# UFC-57 — Career: null write en +0x1a3d204 (hilo "UI Update"), GitHub #25, #16, #18

## Qué pasa en el juego (estático, eboot 1.14)

- **Builder de snapshots** `+0x1a3d100(scene)`: toma el lock del scene
  (`pthread_mutex_lock([scene+0x48]+0x10)`, llamada en `+0x1a3d140`, único sitio), libera los
  snapshots terminados (`scene+0xf0`), pasa el snapshot anterior a la cola para el render
  (`scene+0xc0`) y copia **todos** los nodos del scene en bloques de 0x1e8 bytes
  (`[scene+0x18]->vtbl+0x58`, `+0x1a3d442`). Si el allocator devuelve NULL lo guarda igual en
  `node+0x20`; el build siguiente escribe a través de él: `+0x1a3d204`.
- **El allocator** (`scene+0` y `scene+0x18`) es el allocator global del juego (`[0x2b6c198]`,
  EA MemoryFramework). Para la UI va al heap `FRONTEND_GEN` (HighLow8k, **320 MiB**) de
  `samplegame/data/common/config/memory.cfg`, dentro del contenedor `kettle` de 2300 MiB.
- **El consumidor** es el draw de la vista UI en el render (`+0x1b95030 → +0x1b950a0 →
  +0x1a3be50 → +0x1a3bf70`): toma de a uno los snapshots en cola y los aplica, en bucle,
  hasta vaciar la cola. Un frame de render libera todo lo acumulado.
- **El productor** es el job "UI Update" que el bucle principal lanza en cada tick del FE
  (`+0x578590`, dt fijo 1/30 s) y espera (semáforo EA en `obj+0x450`, `sceKernelPollSema` y después
  `sceKernelWaitSema` sin timeout). **Nada hace esperar al productor por el render.**
- `[mgr+0x18]->vtbl+0x68` / `vtbl+0x60` (antes de cada build, `+0x19f074c`) **no son una espera
  contra el render**: el objeto es la clase de vtable `0x2547288`; `+0x68` (`0x1881d60`) devuelve
  `[[this+0x48]+0x20]+0xe4 != 0` y `+0x60(bool)` (`0x1881d40`) suma/resta 1 a ese byte. Es un
  contador de anidamiento (begin/end de un bloque de actualización), sin ninguna primitiva del
  sistema detrás.

Conclusión: los snapshots solo se acumulan si el render deja de dibujar la UI durante muchos
ticks del juego. En la PS4 eso no pasa; en el port sí: en #16 dos hilos del juego llevaban
**5.5 s esperando un readback del hilo de comandos GPU** cuando el UI Update se quedó sin heap.
Career después de un título (fondo nuevo, #18 "heavy lag") es donde el render más se frena.

## Lo que se descartó

- **Heap chico por el HLE** (hipótesis 2): el tamaño lo fija `memory.cfg` (igual que en la PS4) y
  el contenedor se pide con `sceKernelAllocateDirectMemory(…, size=2300 MiB)`; el log de #25
  muestra que se entrega entero (`direct allocation offset=0x200000, size=2411724800`).
  `sceKernelGetDirectMemorySize` devuelve 5056 MiB como la PS4. No hay diferencia con la consola.
- **Primitiva mal emulada en la espera del builder**: no hay espera; ver arriba.

## El arreglo (src/runtime_ui.c)

Contrapresión del render al builder, en el runtime, sin tocar el código del juego:

- `posix_mutex_lock` reconoce la llamada del builder por su dirección de retorno
  (`imagen+0x1a3d145`, activado solo si los bytes de `+0x1a3d134` son los del eboot 1.14).
- Cada flip del juego (`sceVideoOutSubmitFlip` y el flip de `sceGnmSubmitAndFlipCommandBuffers`)
  llama a `runtime_ui_guest_flip()`.
- Un scene que ya se construyó 8 veces desde el último flip espera, antes de tomar su lock, al
  flip siguiente (como mucho 250 ms; si no llega, pasa un build y el siguiente vuelve a esperar).
  Con el render andando nunca se llega a 8 builds por flip; con el render parado el juego hace
  ~4 builds por segundo en vez de 30–240.
- `BB_UI_BACKPRESSURE=0` lo apaga; `BB_UI_BACKPRESSURE_BUILDS` y `BB_UI_BACKPRESSURE_MS` lo ajustan.
- En el log: `UI back-pressure (UFC-57): on…` al arrancar, y `… N UI builds waited for the
  renderer …` la primera vez y cada 256 esperas; al salir, el total en el informe del runtime.

## Prueba

Career con un save avanzado (después de ganar un título / el Super Fight). Antes: crash al entrar
(#25, 100 %) o al volver de la pelea del título (#16). Ahora: entra; si el render se frena, el log
muestra las esperas de `UI back-pressure`. Si aun así crashea en `+0x1a3d204`, el informe UFC-57
del fault (primera vuelta) dice cuántos snapshots había en cola: con la cola corta, el heap se
llenó por otra cosa y el arreglo es otro.

## GitHub #26 (UFC-58, RenderThread, `+0x402882`)

Mismo patrón, otro pool: `+0x402836` saca una entrada de un pool de capacidad fija (lista libre
`this+0x1a0`, después una zona contigua `this+0x1a8..+0x1b0`) y, si no queda, usa NULL sin
chequear (`+0x402882`). Antes de pedir, recicla de la lista `this+0x188` solo las entradas con
`[e+0x30] == 0` (todavía "en uso" no vuelven). Si ese byte lo limpia la finalización de la GPU
(un label/EOP del frame), la causa de fondo es común en espíritu —un consumidor atrasado, aquí la
GPU emulada y no el render— pero **este arreglo no la cubre**: la espera de UFC-57 frena al
builder de la UI, no a quien llena este pool. Falta confirmar quién pone `[e+0x30]` a 0 (UFC-58).
