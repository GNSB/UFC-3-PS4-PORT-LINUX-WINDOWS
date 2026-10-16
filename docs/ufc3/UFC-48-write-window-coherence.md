# UFC-48: write windows coherentes

Estado: causa identificada en código y vista en el log de Windows; arreglo implementado y
probado con un test unitario de la lógica de merge. **No validado en juego** (no se ejecutó).
Defaults de release sin cambios: `UFC3.exe` sigue con `BB_WRITE_WINDOW=0 BB_WRITE_STEP=0`; la
prueba usa un segundo launcher, `UFC3-WRITE-WINDOWS-TEST.exe`.

Base: `ufc3-online` + UFC-47 (late-label guard, para no mezclar síntomas) + UFC-2 (el audit
`BB_WRITE_WINDOW_DIAG` que produjo el log y su documento).

## Causa

Una window abre la página entera (4 KiB) a la CPU sin readback, aunque tenga bytes que escribió
la GPU (`gpu_modified_ranges` = R: VRAM tiene el valor bueno, el guest está viejo).
`DecideWriteStep` sólo comprueba que el *primer* write esté a más de 16 bytes de R; con la
página abierta cualquier thread escribe cualquier byte, incluidos los de R.

1. **Cierre (`CloseWindows` → `NoteSteppedWrite` → `ProcessLateWrites`)**: las palabras que la
   CPU cambió y que tocan R se **descartaban** (`stepped_write_conflicts`) y quedaban en R. El
   guest tiene el valor de la CPU, R dice «GPU-owned».
2. **Siguiente readback de la página (`DownloadMemory` → `TryWriteBacking`)**: copia R desde
   VRAM al guest → **pisa la escritura de la CPU** con el valor viejo de VRAM.
3. Variantes del mismo hueco: un readback *durante* la window (o en vuelo mientras se cierra)
   escribe VRAM sobre bytes que la CPU acaba de escribir sin fault.

Si esos bytes son enlaces de la free list del allocator (memoria que la GPU escribió, se liberó
y el allocator reusa como nodos), el «next» queda con datos GPU → `mov rax,[r13]` en
`+0x1262a2a` con `r13=-1` (UFC-2).

### Evidencia en el log (UFC2-diag-logfolder-win-20261016_160516.log, Windows RTX 3060)

- Líneas 2795–2821: 24 `upload-skip` en la página `0x100077e000`, `opener=0x1262c9f`
  (el allocator, vecino del fault `0x1262a2a`), `caller=+0xf409b`. Los valores son una free
  list de nodos de 32 bytes: `0x...ec20 → 0x100077ec40`, `ec40 → 0x100077ec00`,
  `ec60 → 0x100077ec80`, `ee60 → 0x100077ecc0` (la mitad alta de cada puntero, `payload=0x10`,
  es la palabra siguiente). Son escrituras del allocator hechas por la window sobre bytes en R,
  **descartadas** al cerrar: el paso 1 de arriba, sobre la free list.
  El ticket los leía como «limpios» por `changed_since_snapshot=0`, pero ese campo compara con
  el snapshot *de cierre*, que ya contiene esas escrituras: no dice nada de si se subieron.
- El audit viejo **no podía ver** el paso 2 sobre esa página: `readback-overwrite` exigía
  `guest != snapshot de cierre`, y en el paso 2 el guest todavía es igual al snapshot (es la
  escritura de la CPU) cuando el readback la pisa.
- Los 64 `readback-overwrite` reportados son casi todos **falsos positivos**: 62 en
  `0x10013fd160+4` (un contador que escribe la GPU, `opener=0x12590c2`) forman una cadena exacta
  `guest(n) == payload(n-1)` (comprobado sobre las 62 líneas, 0 cortes): es un readback que pisa
  al readback anterior, no a la CPU. Los dos restantes (`id=3022` en `0x1000aff000`,
  `id=138221` en `0x1001338000`) son floats y no se pueden atribuir con ese audit. Además el tope
  de 64 se agotó con esa cadena (línea 2996).
- Contexto, no prueba: en la carga posterior a la pelea (`EntityLoader` creado en 2954–2956; stats en 2952–3192) los
  conflicts suben a 6881, 3993 y 27541 por ventana de 5 s y los FPS caen a 7,5 antes del freeze.

### Por qué Windows y no (o menos) Linux

**No determinado.** El camino de código es el mismo en las dos plataformas (no depende de
VEH/VirtualProtect frente a mprotect: el write se pierde por la regla de ownership, no por la
protección). En Linux las windows también estaban activas cuando se midió 137–196 FPS; sin un
log de Linux con el audit no se puede decir si allí los writes se pierden menos (timing,
reutilización de la memoria) o se pierden sin crashear. El arreglo es independiente de la
plataforma.

## Arreglo

Regla: cada submission cierra todas las windows antes de que la GPU la vea, así que toda GPU
write que una window pueda tocar se envió **antes** de que la window abriera. Un byte que la CPU
cambió en la window es el valor más nuevo: gana la CPU.

Por granos de **8 bytes alineados** (`WindowMerge::Granule`): un puntero cambiado en parte (la
mitad alta igual al valor viejo del guest) es todo de la CPU. Los bytes GPU que el juego tiene
junto a sus datos CPU están a más de 16 bytes (gate de `DecideWriteStep`).

| Archivo | Cambio |
| --- | --- |
| `gpu/shadps4/video_core/write_window_merge.h` (nuevo) | Lógica pura: `ForEachChangedRun` (diff de cierre por granos) y `ForEachWritableRun` (qué bytes de un readback se escriben) |
| `page_manager.cpp` `CloseWindows` | Todo el cierre bajo `windows_mutex` (antes soltaba el lock antes de reproteger y diffear: un fault podía abrir otra window encima, y un readback no veía ni la window ni su diff). Los granos cambiados van a `NoteSteppedWrite(..., from_window=true)` |
| `page_manager.cpp` `ReadbackSnapshot` / `WriteReadback` (nuevos) | El readback toma al empezar los bytes sobre los que puede escribir (en páginas con window, el `before` de la window); al escribir salta los granos que cambiaron desde entonces o desde que abrió la window. En una página con window abierta la pone read-only durante el merge (un write de la CPU en ese momento falla, espera `windows_mutex` y se reintenta) y actualiza el `before` con lo que escribe (no es un write de la CPU: el cierre no lo sube como tal) |
| `buffer_cache.cpp` `NoteSteppedWrite` | `from_window`: resta esos bytes de `gpu_modified_ranges` en el momento (todavía bajo el lock de la window) y los encola aparte (`window_writes`) |
| `buffer_cache.cpp` `ProcessLateWrites` | Los granos de una window se suben a VRAM enteros, toquen bytes GPU o no. El single-step (`stepped_writes`) conserva la regla anterior (descartar conflictos): no se cambió su comportamiento |
| `buffer_cache.cpp` `DownloadMemory` | Escribe a través de `PageManager::WriteReadback` con el snapshot de inicio (sólo si windows+step están activos y twins no) |
| Stats | `Write windows: N/s opened instead of a readback; CPU took over X GPU bytes, readbacks kept Y CPU bytes (UFC-48)` |
| Audit (`BB_WRITE_WINDOW_DIAG`) | Tope configurable `BB_WRITE_WINDOW_DIAG_MAX` (default 64). `readback-overwrite` redefinido: un readback (ya escrito) dejó el payload sobre bytes que la window cambió, y no lo que la window dejó; el snapshot sigue a los readbacks (se acaban las cadenas readback-sobre-readback) y los bytes que escribió un readback durante la window no cuentan como cambio de la CPU. Campos nuevos: `window_changed`, `overwritten`, `window_left` |
| `packaging/windows/sacredfire_launcher.c` | `#ifdef UFC3_WRITE_WINDOWS_TEST`: fuerza `BB_WRITE_STEP=1 BB_WRITE_WINDOW=1 BB_WRITE_WINDOW_DIAG=1 BB_WRITE_WINDOW_DIAG_MAX=1000`, título y línea de log propios. Sin el define (el `UFC3.exe` del zip) no cambia nada |
| `tests/test_write_window_merge.cpp` (`write-window-merge-test`) | Diff por granos, merge, y un modelo del caso del log: con la política anterior el enlace `0x100077ec40` termina en `0xffffffffffffffff`; con la nueva sobrevive al cierre, a un readback durante la window y a uno en vuelo a través del cierre |

### Límites conocidos

- Un write de la CPU que deja **exactamente** el valor viejo del guest en un grano entero no se
  ve en ningún diff: ese grano sigue en R y un readback lo pisa. Es indetectable sin fault; los
  granos de 8 bytes lo reducen (antes era por byte en la práctica) pero no lo eliminan.
- Una escritura de host al guest durante una window (labels del CP, readbacks de imágenes) se
  trata como de la CPU: pasa a VRAM y sale de R. El guest es la copia correcta en esos casos.
- Una GPU write grabada en el GPU thread *después* del cierre y antes de `ProcessLateWrites`
  sobre los mismos bytes queda ordenada por la cola (gana la que se ejecuta después), igual que
  antes para los bytes fuera de R. Sólo pasa si el juego escribe con la CPU memoria que una
  submission anterior todavía está escribiendo sin esperarla (una carrera del propio juego).
- El single-step conserva su regla de descartar conflictos (y su regresión conocida de pieles
  negras en peleadores creados, por eso la prueba es con peleadores de fábrica).

## Compilación

- Linux: `ufc3-build` → exit 0 (PGO off, LTO ON). `write-window-merge-test` y
  `label-guard-test` (UFC-47) pasan.
- Windows: `scripts/build-windows.sh --runtime` (toolchain/sysroot/deps/downloads enlazados del
  checkout principal, bajo el lock de `ufc3-build`) → exit 0, `ufc3-v0.1.4-25-gdfc7705`.
  `scripts/package-windows.sh` limpio. El launcher de prueba se compiló aparte con los mismos
  comandos de ninja más `-DUFC3_WRITE_WINDOWS_TEST`, se le quitó la info de debug y se agregó al
  zip como `UFC3-WRITE-WINDOWS-TEST.exe` junto con `README-UFC48.txt`.
- Paquete: `~/UFC3Native-windows-ufc48-dfc7705.zip` (+ `.sha256`).

## Una prueba

Paquete: `~/UFC3Native-windows-ufc48-<commit>.zip`. Abrir **`UFC3-WRITE-WINDOWS-TEST.exe`**
(no `UFC3.exe`); el título dice «UFC-48 write windows test». Misma corrida que UFC-2: Fight Now
con peleadores de fábrica → pelea completa → post-pelea → volver y cargar la segunda; hasta 20
min o el primer crash/freeze. Mandar el log completo.

Qué buscar en el log:
- Línea `UFC-48 write windows test: ...` y `Write window audit: on; ... at most 1000 reports`.
- `Write windows: N/s opened instead of a readback; CPU took over X GPU bytes, readbacks kept Y`
  → N > 0 confirma windows activas; X > 0 son los casos que antes se perdían (ahora se suben).
- `Write window audit: readback-overwrite` → **debe haber 0** (o casi: un caso legítimo es la
  GPU reescribiendo después bytes que la CPU cambió en una window; mandar cualquiera que salga).
- `Write window audit: upload-skip` → con windows ya no se descartan; si aparecen, son de
  single-steps (correlacionados por página con la window más reciente).
- `Stepped writes: ... conflicts` → ahora cuenta sólo los del single-step; no debe dispararse a
  miles en la carga de la segunda pelea como en el log anterior.
- `Frame stats: X FPS` durante la pelea frente al build actual (0.1.4 / UFC-47, windows
  apagadas). En el log de UFC-2 con windows la pelea iba a 109–192 FPS.
- Un fault en `+0x1262a2a` o un freeze con este build refuta el arreglo como suficiente: mandar
  el log (el audit imprime el `Write window audit fault` con r12–r15 y la cabeza del allocator).
