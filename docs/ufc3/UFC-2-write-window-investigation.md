# UFC-2: corrupción del heap con write windows

Estado: allocator corrupto confirmado; causa exacta del write todavía no demostrada.
Se entrega diagnóstico optativo, no un fix de coherencia ni una habilitación por defecto.
No se ejecutó el juego. La segunda pasada (abajo) corrige qué builds tenían windows activas
y reemplaza la prueba manual de la primera.

## Segunda pasada (16 oct): qué builds abrían windows de verdad

### Hechos (código y tags)

- `BufferCache::DecideWriteStep` retorna `false` cuando `BB_WRITE_STEP=0` (o con twins, o
  `assume_locks`) **antes** de mirar `WriteWindowsEnabled()` y antes de `RequestWindow()`. Con
  step apagado no se abre ninguna window: `BB_WRITE_WINDOW=1` sola es inerte.
- El banner `CPU writes beside GPU data open their page...` lo imprime `WriteWindowsEnabled()`
  en el primer write fault aunque step esté apagado (el handler lo consulta antes de llamar a
  DecideWriteStep). El banner **no** prueba windows activas; la prueba es la línea de stats
  `Write windows: N/s opened instead of a readback` (contador `window_opens`, que sólo sube
  después del gate). Este commit hace que el banner diga `inactive: BB_WRITE_STEP=0 opens none`.
- El código de windows/step/readback (`page_manager.cpp`, `buffer_cache.cpp`, `liverpool.cpp`)
  es igual en `ufc3-v0.1.2`, `ufc3-v0.1.3`, `ufc3-v0.1.4` y `ufc3-online` (8de5098), salvo el
  diagnóstico de este ticket y el CMask de PS4 Pro. Lo que cambia entre tags es el entorno:

| Build | Windows launcher | Linux run.sh | ¿Windows abren? |
| --- | --- | --- | --- |
| 0.1.2 | WINDOW=1, STEP sin fijar (default on) | — | **Sí** |
| 0.1.3 | WINDOW=1, STEP=0 | WINDOW=1, STEP=0 | No |
| 0.1.4 / ufc3-online | WINDOW=0, STEP=0 | WINDOW=1, STEP=0 | No |

- Consecuencia: el hotfix 0.1.4 (`BB_WRITE_WINDOW=0` en Windows) no cambió el comportamiento
  respecto de 0.1.3: en ambos ya estaban inactivas. Hoy, ni Linux ni Windows abren windows ni
  hacen single-step por defecto: todo write fault sobre una página GPU paga un readback.

### Hechos (reportes) cruzados con la tabla

| Reporte | Build | ¿Windows activas? | Fault |
| --- | --- | --- | --- |
| HeuZ (RTX 4090), 3 faults | 0.1.2 | Sí (545–671/s en log) | +0x1262a2a, head `-1` |
| Reddit (RTX 3060) | 0.1.2 | Sí (470–806/s) | +0x1262a2a, head `0x64`, carga 2.ª pelea |
| Tester AMD (RX 9070) | parches sobre 0.1.2 | **Desconocido** (no se sabe si STEP ya era 0) | free-list next `0x64` |
| Amigo (RX 7800 XT), 2 logs | 0.1.3 | **No** (STEP=0) | +0x4c4b3 / +0x4c90d en SimThread, otro sitio |

Los cuatro faults en +0x1262a2a vienen de builds con windows realmente activas. El caso 0.1.3
**no puede atribuirse a windows**: es otro sitio (vecino del allocator) con windows inertes.
Ese log trae además cientos de `Late label guarded: EOS fence ... dropped`, lo que mantiene viva
la hipótesis alternativa de escrituras tardías de fences/labels/readbacks sobre memoria ya
liberada (mismo patrón que el bug de 2026-10-01 en `docs/parallel_gpu.md`: GPU idle señalado
con fences diferidas pendientes, free-list pop leyendo basura).

### Defecto de coherencia de las windows (hecho por lectura de código, no reproducido en juego)

Sea R = bytes de la página en `gpu_modified_ranges` (VRAM es la copia buena, guest es vieja).

1. DecideWriteStep sólo verifica que **el primer** write esté a más de 16 bytes de R. Con la
   window abierta, cualquier thread escribe cualquier byte de la página sin fault, incluidos
   los de R (por ejemplo un bloque que el GPU escribió, se liberó y el allocator reutiliza como
   nodo: el «next» de la free list cae en R).
2. Al cerrar, las palabras que tocan R se descartan (`stepped_write_conflicts`): VRAM conserva
   el valor viejo y R sigue marcado GPU-owned.
3. El siguiente readback que cubra esa página (`DownloadMemory`, ventana de 512 KiB alrededor de
   cualquier fault) copia R de VRAM al guest con `TryWriteBacking`: **pisa el write del CPU**.
   Un readback durante la window abierta hace lo mismo de inmediato.
4. Un write del CPU que deja el mismo valor que tenía la copia vieja del guest no aparece en el
   diff del snapshot: es indetectable, y el readback posterior también lo pisa.

El camino normal (readback antes del write) y el single-step (cada instrucción chequea su
dirección contra R) no tienen el punto 1; el step conserva una versión estrecha (otros threads
durante la instrucción abierta). Este mecanismo explica que el valor leído como «next» sea un
dato GPU (`-1`, `0x64`) y que falle al reciclar memoria (fin de pelea, carga de la siguiente);
**sigue siendo hipótesis**: no se correlacionó el nodo dañado con R.

Carreras secundarias (hechos de código, sin evidencia de que ocurran ni de que corrompan):
- `OpenWindow` cambia protección sin el lock de página de los watchers; si un watcher (p. ej.
  read watcher con `BB_READBACKS=2`) reprotege una página con window abierta, `OpenWindow`
  devuelve «ya abierta» sin reabrirla: el write reintenta y vuelve a fallar hasta el próximo
  submit (spin, no corrupción).
- `CloseWindows` suelta `windows_mutex` antes de reproteger: otro submitter (Gfx/Asc) puede
  seguir sin que los diffs del primero estén encolados para su submit.

### Por qué no hay fix en esta pasada

No existe una corrección mínima demostrable sin juego:
- Subir las palabras en conflicto y quitarlas de R arregla sólo los writes con valor distinto
  (punto 4 queda), necesita uploads con granularidad de byte y el orden entre writes GPU
  registrados después de abrir la window y el write CPU (si no, destruye resultados GPU
  válidos): es un rediseño del ownership, no un parche.
- Una prueba sintética de BufferCache requiere Vulkan/arenas; un modelo aparte sólo repetiría
  la descripción de arriba. No se agregaron tests.
- Serializar Open/Close con los locks de página arregla el spin, no la pérdida de writes.

Recuperar rendimiento sin windows: el camino coherente es el single-step (`BB_WRITE_STEP=1`,
windows 0), apagado por las pieles negras de peleadores creados; no se midió en esta pasada.

### Prueba A/B para el usuario (una sola, Windows)

Mismo build, mismo recorrido: Fight Now con peleadores de fábrica (no created/career, para
no mezclar el problema de pieles de STEP), pelea completa, post-pelea, volver y cargar la
segunda pelea; hasta 20 min o el primer crash. Variables en el entorno **antes** de abrir el
launcher (el launcher respeta las que ya existen):

- **A (baseline, defaults)**: sin variables.
- **B (diagnóstico)**: `BB_WRITE_STEP=1 BB_WRITE_WINDOW=1 BB_WRITE_WINDOW_DIAG=1`

En B el log debe mostrar `Write window audit: on`, el banner sin `inactive` y líneas
`Write windows: N/s`. Entregar ambos logs completos. Lectura:
- B crashea en +0x1262a2a y `Write window audit fault: head page window id=...` encuentra la
  página del head → windows confirmadas como productor; pasar al ticket de rediseño.
- B crashea pero `no retained window for allocator head page` → no confirma windows; mirar
  `upload-skip`/`readback-overwrite` del log.
- A crashea → hay un bug independiente de windows (camino fences/labels, caso 0.1.3).
- Ninguno crashea → no valida nada; comparar FPS de pelea entre A y B como dato de costo.

### Siguiente ticket implementable

Si B confirma windows: «Windows coherentes: reconciliar R antes de dar acceso». Al abrir una
window, descargar sólo los rangos GPU de esa página (readback queue, esperando su write tick)
y restarlos de `gpu_modified_ranges`, de modo que la página quede CPU-owned y el diff de cierre
sea completo; medir FPS contra A. Si A también crashea: ticket aparte sobre fences/labels
tardíos (`Late label guarded`) con `BB_WRITE_LOG=2`.

## Resolución del fault

El worktree no traía `out/guest-image.bin`. Se leyó, sin modificarlo, el del checkout
principal: `/home/myzk/Projects/ufc3-native/out/guest-image.bin` (48.465.196 bytes).
Los offsets coinciden con instrucciones y retornos del stack reportado. Comando reproducible:

```sh
objdump -D -b binary -m i386:x86-64 --start-address=0x12629c0 \
  --stop-address=0x1262ae0 out/guest-image.bin
objdump -D -b binary -m i386:x86-64 --start-address=0x7b410 \
  --stop-address=0x7b510 out/guest-image.bin
```

La función que empieza en `+0x12629c0` redondea el tamaño/alineación solicitado,
selecciona una clase de tamaño en unidades de 32 bytes y extrae un nodo de una lista:

```asm
+0x1262a17 mov r14, [r15+rsi*8+0x18] ; bucket/slab de la clase
+0x1262a1c mov r13, [r14+0x18]       ; cabeza de la free list
+0x1262a20 test r13, r13
+0x1262a23 je   +0x1262a54          ; camino de lista vacía
+0x1262a25 inc  word ptr [r14+0x20] ; contador de asignaciones
+0x1262a2a mov  rax, [r13]          ; FAULT: leer el siguiente nodo
+0x1262a2e mov  [r14+0x18], rax     ; pop de la lista
```

La dirección del fault `FFFFFFFFFFFFFFFF` identifica `r13=-1`. El allocator intenta
seguir una cabeza inválida: confirma corrupción de sus metadatos, aunque no identifica
quién la escribió. Puede provenir de un enlace corrupto de un nodo extraído anteriormente.
Los logs no incluyen r13/r14/r15; no permiten localizar el bucket ni el bloque anterior.
No hay símbolos que permitan dar un nombre C++ exacto.

`+0x7b495` es el retorno de `call [rax+0x38]` en `+0x7b492`. La función
`+0x7b410` recorre allocators y comprueba si devolvieron un puntero no nulo. Es el
dispatcher de asignación que llama al allocator de clases, coherente con el fault.

Los siguientes frames también se resolvieron por sus call sites, sin atribuirles
nombres de símbolos inexistentes:

| Retorno | Instrucción previa y función observable |
| --- | --- |
| +0x11bd0b | `call [r11+0x18]` en +0x11bd07; selecciona allocator según categoría y devuelve puntero/categoría |
| +0xf3f7e | `call [r11+0x10]` en +0xf3f7a; calcula tamaño como stride redondeado a 16 por cantidad y guarda el puntero en objeto+0xa0 |
| +0x116610 | `call +0xf3dc0` en +0x11660b; inicializa el objeto que solicita esa asignación |

El contexto apunta a una asignación de almacenamiento de un objeto/buffer del
engine. Los frames de jobs más profundos varían y no tienen símbolos; no permiten
identificar el productor del write que corrompió el allocator.

Los registros disponibles muestran distintas clases: `rsi=15`, `8` y `1` en los
tres faults. No es una sola petición de tamaño que siempre falle.

## Evidencia original y límites

Se leyó `~/ufcCOntext/EXTERNAL-REPORTS.md` y los seis logs indicados, directamente
desde `/tmp/ufc3z/logs/`. Hay discrepancias con el resumen del ticket:

| Log | Resultado real |
| --- | --- |
| 134426 | Fault en +0x1262a2a, General Job 1, exit 3 |
| 152704 | Sin guest fault; exit 0 |
| 153603 | Fault en +0x1262a2a, Replay Job 3, exit 3 |
| 154238 | Fault en +0x1262a2a, Replay Job 1, exit 3 |
| 141026 | Exit 0 |
| 142452 | Exit 3221226505, precedido por 545 y 671 windows/s |

Los tres faults comparten los primeros cuatro retornos (`7b495`, `11bd0b`, `f3f7e`,
`116610`) y parte del camino de jobs, pero difieren en los frames intermedios.
La última estadística de 134426 incluye 5014 conflictos de stepped writes; la de
152704 incluye 3133 y esa corrida termina limpia. Los otros dos faults tienen cero
conflictos en su última estadística. Ese contador mezcla single steps y windows:
no demuestra por sí solo corrupción ni permite descartar daño anterior.
`0xC0000409` sin subcódigo/dump tampoco prueba por sí solo stack cookie corrupta.

Se revisó `0017-*.patch`: cambia la invalidación de texturas cuando un write GPU
alcanza su interior. No controla la protección, snapshots ni el ownership de los
bytes de una write window. No se aplicó.

## Qué hace el código y causa más probable

1. `BufferCache::DecideWriteStep` comprueba sólo la dirección del fault inicial,
   con margen de 16 bytes, contra los rangos GPU-modified. Para windows no limita
   cuánto podrá escribir después esa instrucción ni otros threads.
2. `PageManager::Impl::OpenWindow` toma un snapshot y concede ReadWrite a toda la
   página de 4 KiB, incluso cuando había read watchers. Cualquier thread puede
   leer bytes GPU obsoletos y escribir bytes GPU-owned sin un nuevo fault.
3. En SubmitGfx/SubmitAsc, `CloseWindows` reprotege, compara en palabras de 4 bytes
   y encola los cambios en `NoteSteppedWrite`.
4. `BufferCache::ProcessLateWrites` descarta cada palabra que toca cualquier rango
   GPU-modified. Incluso bytes CPU adyacentes dentro de esa palabra pueden perder
   su upload. El snapshot no permite distinguir un write CPU de un write host/GPU.
5. El diff sólo hace `runtime.UpdateBuffer` hacia VRAM: no escribe guest. En cambio,
   `BufferCache::DownloadMemory` sí copia a guest mediante TryWriteBacking, usando
   los rangos GPU-modified exactos, sin reconciliación con las windows (salvo el
   experimento separado de twins). Un write CPU invisible dentro de esos rangos
   puede ser sustituido por datos viejos de VRAM en un readback posterior.

La hipótesis principal es pérdida de coherencia/ownership cuando la ventana permite
escrituras CPU posteriores en bytes todavía considerados GPU-owned, por ejemplo
al reutilizar una asignación como nodo libre. También se pierde la barrera de
lectura de esos bytes. **Compartir página con un buffer GPU no basta**: un buffer
readback no sobrescribe automáticamente toda la página ni sus metadatos vecinos.
Falta correlacionar la dirección dañada con el rango GPU y el write concreto.

Hay además una carrera real de cierre: `CloseWindows` retira la lista bajo
`windows_mutex`, suelta ese mutex y luego reprotege y toma el snapshot final. Un
fault concurrente puede abrir otra window para la misma página mientras se cierra
la anterior, habilitando escrituras durante la captura o recibiendo después la
reprotección de la window anterior. OpenWindow ni siquiera usa el lock de página
que emplean los watchers. Submitters concurrentes también pueden solapar cierres.
La reprotección por sí sola no garantiza el comentario «none is lost» del código.

No se aplicó un fix especulativo: subir palabras en conflicto puede destruir
resultados válidos del GPU; ignorarlas puede perder writes CPU; serializar sólo
OpenWindow/CloseWindows no resuelve lectores de datos obsoletos, watchers ni writes
por el backing mapping. Un fix general requiere ownership/rangos de descarte
confirmados, o reconciliar con GPU antes de permitir esos accesos. No se recomienda
reactivar windows por defecto con esta evidencia.

## Parche de diagnóstico

`BB_WRITE_WINDOW_DIAG=1`, apagado por defecto, conserva hasta 1024 snapshots de
windows activas/en cierre/recientes (aprox. 4 MiB de datos, más estructuras):

- `reopen-during-close`: apertura observada mientras otra window de esa página
  todavía está cerrándose.
- `upload-skip`: palabra cuyo upload se descarta por conflicto GPU, correlacionada
  con la window más reciente retenida de esa página.
- `readback-overwrite`: buffer readback cuyo payload difiere de un byte guest que
  cambió desde el snapshot. Se registra antes de escribir al backing.
- Cada evento incluye página, ID/edad en aperturas, fase, opener/caller, thread,
  bytes distintos, qword del snapshot, qword guest y payload con sus direcciones.
- En un fault se imprimen RIP y r12–r15. Para +0x1262a2a también se lee mediante
  backing la cabeza `[r14+0x18]` y se busca su snapshot reciente. La consulta del
  historial usa try_lock para no esperar un audit interrumpido.

Máximo 64 líneas por clase de evento. `opener` es un offset guest o una dirección
host con el bit alto de HostSite; `caller` es un offset guest. Baseline/guest son
qwords alineados a 8 bytes; payload empieza en `payload_at` y contiene hasta 8 bytes
de ese rango, cero extendidos. No deben compararse sin atender a las direcciones.
En el dump del fault las fases numéricas son 0=open, 1=closing, 2=closed.

Archivos: `page_manager.cpp/.h` implementan el historial; `buffer_cache.cpp` añade
los dos puntos de observación; `gpu/shim/bbport_write_log.cpp` conecta el reporte
de fault existente. No cambia decisiones de upload/readback/protección ni defaults.

Limitaciones: un cambio de snapshot puede ser CPU, GPU o host; una página histórica
puede haber sido reutilizada/remapeada. Los eventos son candidatos, no prueba de
ownership ni de que causaron el fault. Las fases son observaciones concurrentes,
no un reloj global del pipeline. No se interceptan escrituras directas GPU en
memoria in-place ni readbacks de imágenes/labels. El historial y los reportes son
acotados; su ausencia no descarta el bug y el coste del diagnóstico puede cambiar
el timing de la carrera.

## Una prueba manual

Ejecutar una sola sesión con el build que contiene este parche:

```sh
BB_WRITE_WINDOW=1 BB_WRITE_STEP=1 BB_READBACKS=2 BB_WRITE_WINDOW_DIAG=1 ./run.sh
```

En Windows, establecer esas mismas cuatro variables en el entorno del proceso que
ejecuta el juego antes de abrirlo. Mantener el resto de opciones de la reproducción
original y comprobar en el log el banner `Write window audit: on` y actividad
`Write windows:`. Jugar una pelea completa, ver la repetición/postpelea y comenzar
la segunda; continuar hasta 20 minutos o el primer crash. Entregar el log completo,
incluyendo `Write window audit` y el reporte del fault. No activar BB_WRITE_LOG:
su dump antiguo supone otro allocator y lee directamente un bloque desde rax.

Objetivo: correlacionar el bucket/nodo con los eventos de overwrite/skip y determinar
si se observa reapertura durante cierre. Una sesión sin crash no valida un fix.
Queda pendiente esa evidencia para diseñar y validar la corrección de coherencia.

## Compilación

`ufc3-build` terminó con exit 0: biblioteca GPU (PGO off, LTO ON) y
`out/sacredfire` generados. Hubo warnings de políticas CMake y de ODR en los tipos
FidelityFX; no hubo errores de compilación. `git diff --check` pasó.

El primer intento esperó el lock global y falló antes de compilar porque build.sh
está versionado con modo 0644 y el wrapper lo ejecuta directamente. Se habilitó
temporalmente su permiso de ejecución para repetir `ufc3-build` y se restauró 0644
al terminar. También se revirtió únicamente el parche FSR que build.sh aplica
automáticamente al submódulo, para dejar los cambios del ticket aislados.

No se ejecutó el binario, el juego ni pruebas largas. No se validó el build Windows;
la instrumentación usa el acceso a registros multiplataforma ya existente.

### Compilación (segunda pasada)

Cambio de código: sólo el texto del banner de `BB_WRITE_WINDOW` en `page_manager.cpp`
(`getenv`/`printf` portables); no cambia gates, defaults ni decisiones.

- Linux: `ufc3-build` → exit 0, `out/sacredfire` generado (PGO off, LTO ON).
- Windows: **no verificado**. No hay wrapper Windows con lock; `scripts/build-windows.sh
  --runtime` reconstruiría toda la biblioteca en el worktree. Se preparó una comprobación de
  una sola unidad sin reconstruir (`ninja -t commands sacredfire-game` del build Windows del
  checkout principal, filtrando `page_manager.cpp`, con rutas `gpu/` apuntando al worktree y
  `-fsyntax-only` en lugar de `-o`), pero el entorno del agente no permitió ejecutarla.
- `build.sh` volvió a aplicar el parche FSR al submódulo `gpu/third_party/fsr-vulkan`; queda
  modificado en el worktree y no se commitea (el agente no pudo revertirlo).
