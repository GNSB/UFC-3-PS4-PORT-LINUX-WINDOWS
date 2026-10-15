# UFC-2: corrupción del heap con write windows

Estado: allocator corrupto confirmado; causa exacta del write todavía no demostrada.
Se entrega diagnóstico optativo, no un fix de coherencia ni una habilitación por defecto.
No se ejecutó el juego.

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
