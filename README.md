# UFC 3 — native PS4 port for Linux and Windows

**EA SPORTS UFC 3** (PlayStation 4, `CUSA06534`, update **1.14**) running as a **native x86-64**
program on Linux and Windows. This is not a CPU emulator: the game's original code runs directly
on the PC's processor, a dedicated runtime replaces the PS4 system libraries, and a renderer
derived from [shadPS4](https://github.com/shadps4-emu/shadPS4) translates the PS4's GPU work to
Vulkan.

It is built on [SacredFire](https://github.com/T3CHNOLOG1C/SacredFire) (Crash Team Racing:
Nitro-Fueled), which itself comes from [bbport](https://github.com/deadinside28/bloodborne_pc),
the native Bloodborne port. All code is GPL-2.0-or-later.

> **No game files are included.** You need your own dump of UFC 3 (`CUSA06534`) and its 1.14
> update. This project is not affiliated with or endorsed by Electronic Arts, Sony Interactive
> Entertainment or the UFC. *EA SPORTS UFC*, *PlayStation* and related marks belong to their
> owners and are named only to identify the game.

---

## Status

**Playable.** Menus, loading, fights (including the menu's attract-mode fight), sound, controller
and saves.

| Scene | Linux · Ryzen 5 5600 + RTX 4070 | Windows · Omen 16 (RTX 3060 Laptop, 6 GB) |
|---|---|---|
| Menus / loading screens | 240 FPS (the game's own cap) | 240 FPS (*Immediate* present mode) |
| Fight, no FPS limit | **137–196 FPS** | 66–83 FPS before write windows; the new build is not measured yet |
| Fight, 120 FPS limit | steady 120 FPS | — |

- Tested on NVIDIA (Linux and Windows). The AMD code paths are SacredFire's (developed on AMD /
  Steam Deck), but **UFC 3 has not been tested on an AMD GPU yet**.
- Known issues: moving the window to another monitor *during startup* can crash the NVIDIA driver
  (`Swapchain::Destroy`).

## Requirements

- x86-64 CPU with AVX (the game code, built for the PS4's Jaguar, uses AVX).
- GPU with Vulkan 1.3. ≥ 6 GB of VRAM recommended (below that a more aggressive texture
  collector kicks in).
- A dump of the game: the `CUSA06534` base folder and the 1.14 update, for example:

  ```
  PS4/CUSA06534          (base game, eboot.bin)
  PS4/CUSA06534-patch    (1.14 update; -PATCH, -update, _patch… also work)
  ```

  `scripts/merge_update.py` merges them automatically into `CUSA06534-merged` using *hard links*
  (no extra disk space; on drives without hard links the files are copied).

## Windows

1. Download the zip from [**Releases**](../../releases) (or build it, see *Building*) and extract it.
2. Open **`UFC3.exe`**. Pick the `CUSA06534` folder (the base game; an update next to it is
   detected automatically) and press **Play**.
3. On first launch the game is prepared (a few seconds) in `%LOCALAPPDATA%\UFC3Native`. Each
   session's log goes to `%LOCALAPPDATA%\UFC3Native\logs\`.

### Launcher settings (God of War III port style)

| Setting | Values | Notes |
|---|---|---|
| Player name | text (16 ASCII characters) | The name the game shows. |
| Controls... | per PS4 input | Keyboard key (click, then press it) and gamepad button. Saved to `bbport.ini`. |
| Game resolution | 720p / 1080p | What the game renders. |
| Display resolution | Same / 1080p / 1440p (2K) / 4K | Window size; the game's 1080p frame is scaled to it (use FSR 1.0 + sharpening). Fullscreen uses the monitor's size. |
| FPS limit | 60 / 120 / 144 / 240 / **Unlimited** | The game never goes above 240. |
| Present mode | Mailbox / VSync (FIFO) / **Immediate** | On NVIDIA, Mailbox stops at 2× the monitor's refresh rate; Immediate reaches 240 (may tear). |
| Upscaling | Off / AMD FSR 1.0 | For an internal resolution below the monitor's. |
| Sharpness (RCAS) | Off / Low / Medium / High | |
| FPS counter | on / off | Top left. |

Environment variables you already have set take precedence over the launcher.

## Linux

```bash
# dependencies: C/C++ compiler, CMake, Vulkan, SDL3 and Python 3 (see shell.nix / build.sh)
git clone <this repo> ufc3 && cd ufc3
git submodule update --init --recursive
bash build.sh                                       # builds the runtime + renderer
python3 scripts/merge_update.py /path/PS4/CUSA06534 # base + 1.14 -> /path/PS4/CUSA06534-merged
BB_GAME_DIR=/path/PS4/CUSA06534-merged bash run.sh  # defaults to orig/CUSA06534
```

`run.sh` detects the `ufc3` profile and applies the game's defaults; any variable can be
overridden on the command line (`BB_FPS_LIMIT=0 bash run.sh`).

## Building the Windows zip (on Linux)

```bash
bash scripts/build-windows.sh                 # llvm-mingw cross-compile, LTO on
BB_ALLOW_DIRTY=1 bash scripts/package-windows.sh
# -> dist/UFC3Native-windows-x86_64.zip  (UFC3.exe, bin/, embedded Python, scripts/)
```

### Publishing a release

Write the notes in `docs/ufc3/releases/<version>.md`, commit, then:

```bash
bash scripts/release.sh 0.1.0   # tags ufc3-v0.1.0, builds, packages, pushes, creates the GitHub release
```

## Key environment variables

| Variable | UFC 3 default | What it does |
|---|---|---|
| `BB_FPS_LIMIT` | 0 on Windows (unlimited; the launcher offers 60/90/120/144/240), 120 in `run.sh` | FPS limit (0 = none). |
| `BB_NEO` | 0 (launcher: "PS4 Pro mode") | PS4 Pro mode: the game renders 1080p instead of 900p. |
| `BB_PRESENT_MODE` | Immediate (Windows) | `Mailbox`, `Fifo` or `Immediate`. |
| `BB_VBLANK_HZ` / `BB_FLIP_RATE` | 480 / 0 | The game asks for 30 Hz; with flip rate 0 and a high virtual vblank it runs at the real rate (the simulation follows real frame time). |
| `BB_READBACKS` / `BB_READBACK_LINEAR` | 2 / 1 | Precise reads of GPU-written memory (without them the fight renders black, as UFC 4 does in shadPS4). |
| `BB_WRITE_STEP` | 0 | CPU writes next to a few GPU-written bytes are single-stepped instead of waiting for the GPU. Off: created/career fighters' skin came out black or garbled. |
| `BB_WRITE_WINDOW` | 0 | Pages shared by CPU and GPU data stay open for the CPU until the game's next submission; then only what changed is uploaded (see *Performance*). Off: the game crashed after the first fight. |
| `BB_PREUPLOAD` | 0 | Background upload of rewritten game memory into VRAM (1, 2 = all of it). Off for the same skin textures as `BB_WRITE_STEP`. |
| `BB_LDS_ORDER` | 1 | Subgroup memory barriers around shared-memory (LDS) stores in translated shaders, which the GCN code expects (octagon floor grain on AMD). `0` turns them off; clear the shader cache when changing it. |
| `BB_GPU_WRITE_TWINS` | 0 | Incompatible with precise readbacks (deadlock). |
| `BB_INTERNAL_WIDTH/HEIGHT`, `BB_FSR1`, `BB_RCAS` | from the launcher | Internal resolution and upscaling. |
| `BB_WINDOW_SIZE` | from the launcher | Window size (`2560x1440`, `3840x2160`…); the game's frame is scaled to it. |
| `BB_SKIP_MOVIES` | 1 | The EA intro and the first-start video play a short black clip (`share/skip_movie.mkv`). |
| `BB_USER_NAME` | `Player 1` | The PS4 user name the game shows (16 bytes). |
| `BB_FRAME_STATS` | 1 in `run.sh` | Periodic statistics in the log (FPS, µs per draw, readbacks, write sites…). |

## How it works

- **Native execution.** `eboot.bin` is converted offline (`scripts/prepare.py`, `link_libc.py`,
  `link_modules.py`) into a flat memory image with the game's own PS4 libc and `libSceFios2`
  linked in as native code. TLS accesses (`fs:`) are rewritten (`gs:` on Linux, the TEB TLS slot
  on Windows).
- **Runtime** (`src/runtime_*.c`): memory, threads, mutexes/semaphores/event flags, files, audio
  (ATRAC9, and MP3 for AJM), controller (SDL), saves, user services and network/online stubs. On
  Windows: SRWLOCK-based mutexes, high-resolution timers, `timeBeginPeriod(1)`, raised priority
  and EcoQoS disabled.
- **GPU** (`gpu/`): an adapted shadPS4 renderer. PM4 commands are decoded on one thread and draws
  are recorded into Vulkan on another (a two-stage pipeline), helped by draw-preparation and
  recording threads. GPU-written memory is page-protected so CPU reads and writes keep it
  coherent.

## Performance: what this port changed

1. **Unlocked frame rate.** The game asks for 30 Hz through its *present interval* function
   (guest `0x1182a0`); the port ignores it and uses a 480 Hz virtual vblank.
2. **Windows on par with Linux.** Windows `Sleep` calls ran on the system's 15.6 ms tick; they
   were replaced with high-resolution timers plus a spin for the last 100 µs (the present loop
   was running at half speed).
3. **`BB_WRITE_STEP`.** A CPU write to a page with ≤ 256 GPU-written bytes runs with the trap
   flag (TF) set, and only the words it changed are uploaded to VRAM, instead of waiting for the
   GPU and downloading the page. Menus no longer sync with the GPU every frame (240 FPS).
   Off by default (with it, created fighters' skin came out black or garbled);
   `BB_WRITE_STEP=1` turns it back on.
4. **`BB_WRITE_WINDOW`** (the big fight improvement). A new per-site counter ("Readback sites" in
   the log) showed that almost every wait came from the EA engine: its dynamic buffers (a lock
   with discard at `0x64b50` → `0xf4120`, e.g. the fighters' 48-byte bone matrices) share pages
   with small shader outputs (e.g. a 408-byte binding at `0x1001338ee0`). Every CPU write forced
   a wait for the GPU and a page download. Now the page stays open for the CPU until the next
   `SubmitGfx`/`SubmitAsc`; it is then protected again, compared with the copy taken when it was
   opened, and only the changed words are uploaded (skipping the GPU's). Result on Linux:
   readbacks 380 → 120 per second and fights from **105–113 to 137–196 FPS**.
   Known risk: if the game read the GPU bytes of an open page, it would see the previous
   submission's data. **Off by default** (Windows launcher and `run.sh`): on an AMD RX 9070 the
   game crashed after the first fight with it on (the guest heap's free list corrupted).
   `BB_WRITE_WINDOW=1` turns it back on for the frame rate above.

The full technical write-up (diagnostics, failed experiments, measurements) is in
[`docs/ufc3/GROK_HANDOFF.md`](docs/ufc3/GROK_HANDOFF.md), [`docs/ufc3/HANDOFF.md`](docs/ufc3/HANDOFF.md)
and [`docs/ufc3/PERF_240_PLAN.md`](docs/ufc3/PERF_240_PLAN.md).

## Diagnostic tools

- `tools/ufc3-iter.sh` — builds, runs with a timeout and summarizes where the run stopped.
- `tools/ufc3-shot.sh out.png` — screenshot of the game window only (Hyprland).
- `BB_WATCH_GPU_WRITE=<addr>[,<addr>…]` — logs which GPU-written bindings cover a page.
- `BB_SKIP_SHADERS`, `BB_SKIP_FILE`, `BB_CAPTURE_ON_FLIP`, `BB_PATCH_SHADERS` — isolate shaders
  and capture frames.
- `BB_DISCARD_HOOK=1` (experimental, Linux) — reports the ranges handed out by the discard lock.

## Credits

- [SacredFire](https://github.com/T3CHNOLOG1C/SacredFire) — the base runtime, renderer and Windows
  packaging (its original README is [`README.SacredFire.md`](README.SacredFire.md)).
- [bbport](https://github.com/deadinside28/bloodborne_pc) — the native port model.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) — renderer and shader recompiler.
- The God of War III Windows port and *eltutz*'s work, as references for Windows tuning (timers,
  priorities, GPU readback model).
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [minimp3](https://github.com/lieff/minimp3),
  [FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR), Dear ImGui.

## License

GPL-2.0-or-later, like bbport, SacredFire and shadPS4 (see [`LICENSE`](LICENSE)).
