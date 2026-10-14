# UFC 3 — PS4 port nativo para Linux y Windows

**EA SPORTS UFC 3** (PlayStation 4, `CUSA06534`, actualización **1.14**) ejecutándose como
programa **nativo x86-64** en Linux y Windows. No es un emulador de CPU: el código original del
juego corre directamente en el procesador del PC, un runtime propio reemplaza las librerías del
sistema de la PS4 y un renderer derivado de [shadPS4](https://github.com/shadps4-emu/shadPS4)
traduce el trabajo de la GPU de la PS4 a Vulkan.

Está construido sobre [SacredFire](https://github.com/T3CHNOLOG1C/SacredFire) (Crash Team
Racing: Nitro-Fueled), que a su vez viene de [bbport](https://github.com/deadinside28/bloodborne_pc),
el port nativo de Bloodborne. Todo el código es GPL-2.0-or-later.

> **No incluye archivos del juego.** Necesitas tu propio dump de UFC 3 (`CUSA06534`) y su
> actualización 1.14. Este proyecto no está afiliado ni respaldado por Electronic Arts, Sony
> Interactive Entertainment ni la UFC. *EA SPORTS UFC*, *PlayStation* y las marcas relacionadas
> pertenecen a sus dueños y se nombran solo para identificar el juego.

---

## Estado

**Jugable.** Menús, carga, peleas (incluida la pelea de demostración del menú), sonido, control
y partidas guardadas.

| Escena | Linux · Ryzen 5 5600 + RTX 4070 | Windows · Omen 16 (RTX 3060 Laptop, 6 GB) |
|---|---|---|
| Menús / pantallas de carga | 240 FPS (el tope del propio juego) | 240 FPS (modo de presentación *Immediate*) |
| Pelea, sin límite de FPS | **137–196 FPS** | 66–83 FPS antes de las ventanas de escritura; pendiente medir el zip nuevo |
| Pelea, límite 120 | 120 FPS estables | — |

- Se probó en NVIDIA (Linux y Windows). El código de AMD es el mismo de SacredFire (que se
  desarrolló en AMD / Steam Deck), pero **UFC 3 no se ha probado todavía en una GPU AMD**.
- Problemas conocidos: ruido/bloques grises en el piso del octágono; los videos tutoriales del
  primer arranque no se pueden saltar; mover la ventana a otro monitor *durante el arranque*
  puede botar el driver de NVIDIA (`Swapchain::Destroy`).

## Requisitos

- CPU x86-64 con AVX (el código del juego, compilado para el Jaguar de la PS4, usa AVX).
- GPU con Vulkan 1.3. Recomendado ≥ 6 GB de VRAM (con menos se activa un recolector de
  texturas más agresivo).
- Dump del juego: carpeta base `CUSA06534` y la actualización 1.14, por ejemplo:

  ```
  PS4/CUSA06534          (juego base, eboot.bin)
  PS4/CUSA06534-patch    (actualización 1.14; también vale -PATCH, -update, _patch…)
  ```

  `scripts/merge_update.py` las une automáticamente en `CUSA06534-merged` usando *hard links*
  (no ocupa espacio extra; en discos sin hard links copia los archivos).

## Windows

1. Descarga o compila el zip (`dist/UFC3Native-windows-x86_64.zip`, ver *Compilar*) y
   descomprímelo.
2. Abre **`UFC3.exe`**. Elige la carpeta `CUSA06534` (la base; la actualización al lado se
   detecta sola) y pulsa **Play**.
3. La primera vez se prepara el juego (unos segundos) en `%LOCALAPPDATA%\UFC3Native`. Los logs
   de cada partida quedan en `%LOCALAPPDATA%\UFC3Native\logs\`.

### Opciones del launcher (estilo port de God of War III)

| Opción | Valores | Notas |
|---|---|---|
| Resolución | 720p / 1080p | Resolución interna de render. |
| Límite de FPS | 60 / 120 / 144 / 240 / **Ilimitado** | El juego no pasa de 240. |
| Modo de presentación | Mailbox / VSync (FIFO) / **Immediate** | En NVIDIA, Mailbox se queda en 2× la tasa del monitor; Immediate deja llegar a 240 (puede haber *tearing*). |
| Escalado | Off / AMD FSR 1.0 | Para usar resolución interna menor que la del monitor. |
| Nitidez (RCAS) | Off / Baja / Media / Alta | |
| Contador de FPS | sí / no | Arriba a la izquierda. |

Las variables de entorno que el usuario ya tenga definidas tienen prioridad sobre el launcher.

## Linux

```bash
# dependencias: compilador C/C++, CMake, Vulkan, SDL3 y Python 3 (ver shell.nix / build.sh)
git clone <este repo> ufc3 && cd ufc3
git submodule update --init --recursive
bash build.sh                                       # compila runtime + renderer
python3 scripts/merge_update.py /ruta/PS4/CUSA06534 # une base + 1.14 -> /ruta/PS4/CUSA06534-merged
BB_GAME_DIR=/ruta/PS4/CUSA06534-merged bash run.sh  # por defecto busca orig/CUSA06534
```

`run.sh` detecta el perfil `ufc3` y aplica los valores por defecto del juego; cualquier variable
se puede sobrescribir en la línea de comandos (`BB_FPS_LIMIT=0 bash run.sh`).

## Compilar el zip de Windows (desde Linux)

```bash
bash scripts/build-windows.sh                 # cross-compile con llvm-mingw, LTO activado
BB_ALLOW_DIRTY=1 bash scripts/package-windows.sh
# -> dist/UFC3Native-windows-x86_64.zip  (UFC3.exe, bin/, python embebido, scripts/)
```

## Variables de entorno importantes

| Variable | Defecto UFC 3 | Qué hace |
|---|---|---|
| `BB_FPS_LIMIT` | 0 en Windows (ilimitado), 120 en `run.sh` | Límite de FPS (0 = sin límite). |
| `BB_PRESENT_MODE` | Immediate (Windows) | `Mailbox`, `Fifo` o `Immediate`. |
| `BB_VBLANK_HZ` / `BB_FLIP_RATE` | 480 / 0 | El juego pide 30 Hz; con flip rate 0 y un vblank virtual alto corre a la tasa real (la simulación usa el tiempo real del frame). |
| `BB_READBACKS` / `BB_READBACK_LINEAR` | 2 / 1 | Lecturas precisas de memoria escrita por la GPU (sin esto la pelea sale negra, igual que UFC 4 en shadPS4). |
| `BB_WRITE_STEP` | 1 | Escrituras de la CPU junto a pocos bytes de la GPU se ejecutan paso a paso en vez de esperar a la GPU. |
| `BB_WRITE_WINDOW` | 1 | Páginas compartidas CPU/GPU quedan abiertas para la CPU hasta el siguiente envío del juego; luego se sube solo lo que cambió (ver *Rendimiento*). |
| `BB_GPU_WRITE_TWINS` | 0 | Incompatible con las lecturas precisas (deadlock). |
| `BB_INTERNAL_WIDTH/HEIGHT`, `BB_FSR1`, `BB_RCAS` | según launcher | Resolución interna y escalado. |
| `BB_FRAME_STATS` | 1 en `run.sh` | Estadísticas cada pocos segundos en el log (FPS, µs por draw, readbacks, sitios de escritura…). |

## Cómo funciona

- **Ejecución nativa.** El `eboot.bin` se convierte offline (`scripts/prepare.py`,
  `link_libc.py`, `link_modules.py`) en una imagen de memoria plana con la libc de la PS4 y
  `libSceFios2` del propio juego enlazadas como código nativo. Los accesos TLS (`fs:`) se
  reescriben (`gs:` en Linux, ranura TLS del TEB en Windows).
- **Runtime** (`src/runtime_*.c`): memoria, hilos, mutex/semáforos/event flags, archivos,
  audio (ATRAC9 y MP3 para el AJM), mando (SDL), partidas guardadas, servicios de usuario y
  stubs de red/online. En Windows: mutex sobre SRWLOCK, timers de alta resolución,
  `timeBeginPeriod(1)`, prioridad alta y EcoQoS desactivado.
- **GPU** (`gpu/`): renderer de shadPS4 adaptado. Los comandos PM4 se decodifican en un hilo y
  los draws se graban en Vulkan en otro (pipeline de dos etapas), con hilos de preparación de
  draws y de grabación. La memoria que escribe la GPU se protege por páginas para que las
  lecturas/escrituras de la CPU la mantengan coherente.

## Rendimiento: qué se hizo en este port

1. **Frame rate desbloqueado.** El juego pide 30 Hz por su función de *present interval*
   (guest `0x1182a0`); el port lo ignora y usa un vblank virtual de 480 Hz.
2. **Windows a la par de Linux.** Los `Sleep` de Windows usaban el tick de 15,6 ms del
   sistema; se reemplazaron por timers de alta resolución + espera activa de los últimos 100 µs
   (el loop de presentación iba a la mitad de velocidad).
3. **`BB_WRITE_STEP`.** Una escritura de la CPU en una página con ≤ 256 bytes escritos por la
   GPU se ejecuta con la bandera de traza (TF) y solo se suben a VRAM las palabras que cambió,
   en vez de esperar a la GPU y descargar la página. Los menús quedaron sin sincronización por
   frame (240 FPS).
4. **`BB_WRITE_WINDOW`** (la mejora grande de las peleas). Con un nuevo contador por sitio
   ("Readback sites" en el log) se vio que casi todas las esperas venían del motor de EA: sus
   buffers dinámicos (lock con descarte en `0x64b50` → `0xf4120`, por ejemplo las matrices de
   huesos de 48 bytes de los luchadores) comparten página con pequeñas salidas de shaders
   (p. ej. un binding de 408 bytes en `0x1001338ee0`). Cada escritura de la CPU forzaba
   esperar a la GPU y leer la página. Ahora la página queda abierta para la CPU hasta el
   siguiente `SubmitGfx`/`SubmitAsc`; ahí se vuelve a proteger, se compara con la copia tomada al
   abrirla y se suben solo las palabras cambiadas (saltando las de la GPU). Resultado en Linux:
   readbacks 380 → 120 por segundo y peleas de **105–113 a 137–196 FPS**.
   Riesgo conocido: si el juego leyera los bytes de la GPU de una página abierta, vería datos
   del envío anterior. Si ves algo raro: `BB_WRITE_WINDOW=0`.

La documentación técnica completa (diagnósticos, experimentos fallidos, mediciones) está en
[`docs/ufc3/GROK_HANDOFF.md`](docs/ufc3/GROK_HANDOFF.md), [`docs/ufc3/HANDOFF.md`](docs/ufc3/HANDOFF.md)
y [`docs/ufc3/PERF_240_PLAN.md`](docs/ufc3/PERF_240_PLAN.md).

## Herramientas de diagnóstico

- `tools/ufc3-iter.sh` — compila, ejecuta con timeout y resume dónde terminó.
- `tools/ufc3-shot.sh salida.png` — captura solo la ventana del juego (Hyprland).
- `BB_WATCH_GPU_WRITE=<dir>[,<dir>…]` — registra qué bindings escritos por la GPU cubren una página.
- `BB_SKIP_SHADERS`, `BB_SKIP_FILE`, `BB_CAPTURE_ON_FLIP`, `BB_PATCH_SHADERS` — aislar shaders y
  capturar frames.
- `BB_DISCARD_HOOK=1` (experimental, Linux) — reporta los rangos que entrega el lock con descarte.

## Créditos

- [SacredFire](https://github.com/T3CHNOLOG1C/SacredFire) — base del runtime, renderer y empaquetado
  de Windows (el README original está en [`README.SacredFire.md`](README.SacredFire.md)).
- [bbport](https://github.com/deadinside28/bloodborne_pc) — el modelo de port nativo.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) — renderer y recompilador de shaders.
- El port de God of War III para Windows y el trabajo de *eltutz*, como referencia de ajustes
  para Windows (timers, prioridades, modelo de lecturas de la GPU).
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [minimp3](https://github.com/lieff/minimp3),
  [FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR), Dear ImGui.

## Licencia

GPL-2.0-or-later, igual que bbport, SacredFire y shadPS4 (ver [`LICENSE`](LICENSE)).
