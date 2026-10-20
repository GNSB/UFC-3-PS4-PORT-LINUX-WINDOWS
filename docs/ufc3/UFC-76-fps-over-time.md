# UFC-76: FPS que cae con el tiempo (issue #47, Steam Deck)

Log: `20261020_090243.log` + `.frames.csv` (v0.1.9, Steam Deck, 80 min, 155.319 frames).
Análisis hecho sobre el CSV de frames y sobre las líneas `Frame stats`, `Growth`, `Memory`,
`Gnm` y `Stall` del log (una línea `Frame stats` cada 5 s).

## Conclusión

Nada del port crece con el tiempo. El FPS depende de la escena y se queda estable durante
70 minutos. A los **~71,5 min (t≈4290 s)** baja de golpe, en unos 20 s, en medio de una
escena estática con el mismo trabajo, y se queda así hasta el final. En ese momento:

- el trabajo del port es el mismo: mismos draws, mismo tiempo de CPU del hilo de la GPU,
  mismo tiempo de kernel y la misma cantidad de llamadas a protect;
- crece solo el tiempo de reloj: el de los handlers de write fault, el de protect, el de la
  espera de host copies y el del frame;
- el hilo de la GPU es **desalojado 2,5 veces más** (15 → 37 preemptions involuntarias por
  frame, mediana de 1610 frames contra 982 de la misma escena).

Eso es CPU que se lleva algo de afuera del juego (otro proceso o el kernel). No es un
throttling de reloj: en ese caso subiría el tiempo de CPU por draw, y no sube (18,7 → 18,1
us/draw). Tampoco es la GPU: la espera del frame anterior no cambia (15,8 → 14,5 ms). El log
de 0.1.9 no trae relojes, temperaturas, batería ni la carga del resto del sistema, así que
no se puede saber qué proceso fue. El arreglo de este ticket agrega esos datos al log.

## Evidencia

### Por fase (CSV, frames agrupados por escena)

| fase | draws/frame | dispatches | FPS | estable en el tiempo |
|---|---|---|---|---|
| menú (0–100 s, 4810 s→) | 12–100 | — | 72 (mediana 13,9 ms) | sí: 13,94 ms al inicio, 13,93 ms al final |
| pelea | 650–850 | 155 | ~36 (27,5 ms) | sí, de 120 s a 4170 s; desde 4380 s, ~30 (32–34 ms) |
| intro / repetición | 1100–1400 | — | 30–35 | — |
| escena estática (pausa/presentación) | ~1700 | 75 | 27,4 (36,5 ms) | sí: 600–1740 s y 2040–2730 s idénticos; desde 4290 s, 40 ms |

Que el FPS "se mueva" entre 27 y 36 a lo largo de la sesión es por la escena, no por el
tiempo: la escena de ~1700 draws cuesta 9 ms más por frame que la pelea.

### La escena estática de ~1700 draws (mismo trabajo en todo el log)

```
  t (s)  frame_ms  CPU hilo GPU  protect  write_fault  kernel
  720      36.53      31.77       18.33     16.59      20.91
 1200      36.50      32.07       18.04     16.18      21.23
 1680      36.51      32.14       17.73     16.04      21.33
 4200      37.45      30.37       19.45     18.55      19.53
 4320      40.72      30.90       20.72     21.11      20.11
```

Cada 10 s alrededor del salto (mediana; gap = frame − CPU del hilo de la GPU):

```
4280  draws 1706  frame 34.7  CPU 29.6  gap 4.6
4290  draws 1703  frame 35.6  CPU 30.6  gap 5.1
4300  draws 1707  frame 39.3  CPU 31.0  gap 8.1
4310  draws 1702  frame 39.8  CPU 31.4  gap 8.6
...   (pelea, ~720 draws)
3850  frame 27.7  gap 4.8      4450  frame 31.7  gap 7.3
```

Antes del salto, el gap es de 4–5 ms en todas las escenas. Después del salto, es de 8–9 ms
en todas.

### Pelea antes y después (CSV, frames con 600–850 draws y 155 dispatches)

```
                 3840–4170 s   4380–4560 s
frame_ms            27.86        32.45
stage_a_cpu_ms      22.89        23.42   <- trabajo del hilo de la GPU: igual
kernel_ms           16.01        16.69
protect_ms           9.85        12.31   <- tiempo de reloj
write_fault_ms       9.41        13.14   <- tiempo de reloj
present_wait_ms      0.60         0.07
draws/submissions  723/19       718/19
```

### Lo que no crece (líneas `Growth` y `Memory`, cada 60 s)

- **Texture cache:** 1902 imágenes y 1250 MiB a los 5 min; 1962 y 1272 MiB al final. El GC
  sigue liberando imágenes viejas (`stale freed` 0–17 por ventana). Collector: ~3,6 de 6,2 GiB.
- **VRAM:** 983–990 MiB toda la sesión (el carveout del Deck es de 1 GiB); GTT 2534 → 2678 MiB.
- **Pipelines:** 397 → 418 gráficos y 79 compute; 0 compilando. `compiles` en el CSV: 0
  después del minuto 4.
- **Buffer cache:** 1 arena; `buffer_mb`/frame ~3 MB constante. Write faults/s: 1100–1300 en
  pelea toda la sesión, sin tendencia.
- **Memoria del guest:** guest blocks en VRAM 256 MiB, la parte sin usar baja de 66 a 41 MiB.
  RSS 4857 → 5031 MiB (+174 MiB en 75 min), sin relación con el salto.
- **Readbacks / write windows:** descartados en UFC-69 (no crecen).
- **Hilos y esperas:** `guest waited for the previous frame` ~12,5 ms en pelea toda la
  sesión. `GPU thread us/draw` ~31 en pelea y ~18,5 en la escena estática, sin tendencia.
- **Throttling térmico:** el log no trae relojes ni temperaturas. El menú del final corre
  igual que al inicio, pero el menú es tan liviano que no lo distinguiría. El patrón (más
  tiempo de reloj con el mismo tiempo de CPU, y 2,5× más preemptions) apunta a CPU tomada por
  otro proceso más que a relojes bajos.

Extra: el entorno del usuario carga una capa Vulkan de terceros, `MAKO Renderer`
(`role=frame-generation`), y el overlay de Steam. Ninguna explica un salto a los 71 min,
pero conviene que la próxima prueba se haga sin la capa.

## Cambio

`gpu/shadps4/video_core/renderer_vulkan/vk_growth_report.cpp`: junto a la línea `Growth`
(cada `BB_GROWTH_SECONDS`, 60 por defecto) se escribe en Linux una línea `System`:

```
System (60 s): CPU busy 71% of 8 threads, this game 58%, others 13%; load 6.2; game preempted 950/s;
CPU 2800 MHz (1400-3500); amdgpu 78 C 14.8 W GPU 1600 MHz, jupiter 70 C; battery 64% Discharging
```

Se lee de `/proc/stat`, `/proc/self/stat|status`, `/proc/loadavg`, `cpufreq`, `hwmon`
(amdgpu, k10temp, jupiter, steamdeck_hwmon) y `power_supply`. Son unas decenas de lecturas
de sysfs cada 60 s. Con esta línea, el próximo log distingue entre otro proceso (sube
"others" y "preempted"), throttling (bajan los MHz o sube la temperatura) y un cambio de
perfil de energía (batería o vatios).

## Cómo probar

Correr cualquier sesión de más de 2 minutos y buscar `^System (` en el log: debe aparecer una
línea por minuto, después de cada `Growth`. En el Deck debería mostrar amdgpu con W y MHz, y
la batería.

## Borrador de respuesta para el issue #47 (inglés)

> Thanks for the 80-minute log, it helped a lot. Nothing in the port grows over time:
> texture cache, VRAM, pipelines, buffers and memory all stay flat, and the same scene runs at
> exactly the same speed at minute 10 and at minute 28. What the log does show is one step
> down at about minute 71: the frame time rose ~5 ms in a few seconds, with the game doing
> the same work, and the game's threads were being preempted 2.5x more often. That looks like
> something else on the Deck taking CPU time (a background task, download or shader
> processing, or the system clocking down). Also, the FPS changing between ~27 and ~36 over
> the session is the scene (fight vs. presentation screens), not time.
>
> The next build logs a `System` line every minute (CPU used by other processes, clocks,
> temperatures, power, battery). If you can, please repeat a long session with that build,
> without the MAKO frame-generation layer, plugged in, and with Steam downloads paused, and
> attach the log again. That will tell us exactly what changes when the FPS drops.
