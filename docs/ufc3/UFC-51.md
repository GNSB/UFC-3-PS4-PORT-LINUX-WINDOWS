# UFC-51 — Release Windows 2e700eb: pelea de Career a ~30 FPS (antes ~68)

Estado: análisis de logs y código, sin cambios de código. Causa identificada con certeza alta.
Falta la prueba de confirmación del usuario (abajo).

Logs (misma PC: RTX 3060 Laptop, 480 Hz, caché de shaders borrada en los dos):
- `release-2e700eb-win-20261016_213336.log`: `UFC3.exe`, PREUPLOAD=1, STEP=0, WINDOW=0.
- `UFC48-test5-win-20261016_210717.log`: `UFC3-WRITE-WINDOWS-TEST.exe` (de4e15e), PREUPLOAD=0,
  STEP=1, WINDOW=1.
- Los logs 0.1.4 de `users-20261016/` no tienen `Frame stats` (y son AMD): no sirven para comparar FPS.

## Números (promedio de ventanas de 5 s)

| | release, pelea (26 vent.) | release, escena previa ~640 draws (13) | test5, pelea Career (27) |
|---|---|---|---|
| FPS / mediana de frame | 29.1 / 34.7 ms | 70.0 / 13.9 ms | 67.4 / 15.4 ms |
| draws/frame, us/draw | 670, 15.2 | 638, 12.1 | 719, 15.7 |
| hilo GPU por frame (us/draw × draws) | **10.0 ms** | 7.8 ms | **11.2 ms** |
| Draw pipe: draws/s, stage B ocupada | 30 600, 23 % | 52 300, 49 % | 74 600, 80 % |
| write faults/s (por frame) | 1084 (37) | 1066 (15) | 1923 (29) |
| **readbacks/s (por frame)** | **312 (10.7)** | 104 (1.5) | **68 (1.0)** |
| Stepped writes / write windows por s | — / — | — / — | 877 / 501 |
| guest esperó al frame previo | 0.9 ms | 3.1 ms | 4.4 ms |
| Replay Job sema, ms/frame | 28 | 13 | 12 |

La post-pelea (release, 2 ventanas, 1166 draws, 2.0 readbacks/frame) vuelve a 54 FPS con *más*
draws: el FPS sigue a los readbacks, no a la cantidad de draws.

## Respuestas

1. **No es costo del hilo GPU.** El hilo GPU trabaja ~10 ms por frame en la pelea del release y
   ~11 ms en la de test5 (que va a 67 FPS); los 15 us/draw son de la escena de pelea (test5 tiene
   15.7). El draw pipe está parado (stage B 23 % contra 80 %) y el juego casi no espera a la GPU.
   Lo que cambia son los **readbacks: 10.7 por frame contra 1.0**. Cada readback es drain del draw
   pipe + esperar a que la GPU termine + bajar la página (GROK_HANDOFF §16): serializa CPU y GPU.
   (34.7 − 15.4 ms) / (10.7 − 1.0) ≈ 2 ms por readback. Sitios en la pelea del release
   (`Readback sites`, por ventana): memcpy del host en 0x100076de20 (~405), `+0x409d0f` (~365),
   memcpy en 0x1038d157c0 (~300), `+0x3f3562` (~143), `+0x3ff5c2`/`+0x3ff254` (~135),
   `+0x12590ae` (~125), `+0x47d855` (~75): los mismos que las write windows quitaron en
   GROK_HANDOFF §17. Pre-upload, compilaciones y texturas no lo explican: pre-upload movió
   0–3.7 MB cada 600 frames; casi todas las ventanas de 30 FPS tienen 0 compilaciones; los
   `texture set misses: generation` son del mismo orden en test5 (80 447 contra ~99 000).
2. **Las ventanas de 30 FPS son la pelea.** Coinciden con la pelea de test5 (≈700 draws, ≈15
   us/draw) y aparecen los sitios de readback propios de la pelea (`+0x3f3562`, ya visto en el
   attract fight). "Replay Job 0-3" son hilos del pool de jobs del juego esperando trabajo en el
   mismo semáforo que "Sim Job" (`+0xddffec<+0x1f14398`). Aparecen también en menús (3.9 ms) y en
   test5 (12 ms). Esos ms crecen con la duración del frame: son síntoma, no causa. No es una
   repetición.
3. **La variable es la falta de write windows (con STEP=0), no PREUPLOAD ni la caché.**
   - Con `BB_WRITE_STEP=0`, `DecideWriteStep` sale antes de decidir nada (`buffer_cache.cpp`,
     `!WriteStepEnabled()` → readback). Toda escritura CPU sobre una página GPU-modificada termina
     en `ReadMemory`. Por eso los sitios dicen `[other, 0 GPU B]`: el motivo ni se calcula. Ese
     mismo gate deja las windows inertes con STEP=0 aunque WINDOW=1 (UFC-2,
     `"inactive: BB_WRITE_STEP=0 opens none"`).
   - PREUPLOAD no entra en ese camino. Sólo sube páginas CPU-modificadas *sin* modificación GPU y
     cambia de dónde salen las texturas. No crea readbacks.
   - Caché fría: test5 también la borró y va a 67 FPS. Además, el FPS se queda en 30 durante
     20+ ventanas sin compilar nada.

Contexto: el launcher de Windows tiene `BB_WRITE_WINDOW=0` desde el hotfix 0.1.4 (386be2d,
corrupción del heap) y `BB_WRITE_STEP=0` desde UFC-49 (por la piel). Entonces 0.1.4 en Windows ya
debería tener este mismo ~30 en peleas de Career en esa PC. El "antes ~68" viene del exe de prueba
con windows, no de un release anterior. test5 (con el arreglo de coherencia de UFC-48, de4e15e,
que ya está en 2e700eb) jugó Career + Quick Fight sin `GPU wait stuck` ni `Device lost` y salió
con código 0.

**Linux también:** `run.sh` exporta `BB_WRITE_WINDOW=1 BB_WRITE_STEP=0` (4b644a8). Con STEP=0 las
windows no abren, así que Linux debería tener la misma pérdida en peleas (las 140–190 FPS de §17
eran con STEP=1).

## Arreglo mínimo propuesto (no implementado)

- **Opción A (config, trivial):** en el launcher de Windows (y en `run.sh`), `BB_WRITE_WINDOW=1`
  y `BB_WRITE_STEP=1`: es justo la config de test5. Riesgos que decide el Jefe: (a) el crash de
  heap que motivó el hotfix 0.1.4 (test5 no lo reprodujo con de4e15e, pero es una sola sesión);
  (b) la piel de UFC-49, que se atribuyó a STEP sin aislarlo. Hay que mirar la piel creada/career
  en la misma prueba.
- **Opción B (código, no trivial):** separar las windows del gate de STEP en `DecideWriteStep`.
  Con STEP=0, las faltas que irían a step o a window abrirían window, nunca un single-step.
  También hay que ajustar el snapshot de `ReadMemory` (`WriteStepEnabled() &&
  WriteWindowsEnabled()`) y el aviso "inactive" de `page_manager.cpp`. Permite WINDOW=1 sin
  tocar STEP, pero cambia la semántica que validó UFC-48: hay que medirlo y auditarlo antes.

## Una prueba (Windows, misma PC, misma pelea de Career)

```
set BB_WRITE_WINDOW=1
set BB_WRITE_STEP=1
UFC3.exe
```

PREUPLOAD queda en 1 (el default del release) y la caché se borra igual que antes. El launcher
respeta las variables del entorno. Predicción: la pelea pasa de ~29 a ~60–70 FPS. En el log, los
`readbacks/s` deberían bajar de ~310 a ~70 y aparecer `Stepped writes`/`Write windows` en cientos
por segundo. Si mejora, quedan descartados PREUPLOAD (sigue en 1) y la caché como causa, y se
confirma la opción A. Mirar también la piel del luchador creado y si hay crash al cargar la
segunda pelea o al firmar contrato. Si sigue en ~30 con los readbacks abajo, la hipótesis cae.
