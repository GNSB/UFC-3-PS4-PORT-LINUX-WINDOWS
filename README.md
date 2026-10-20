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

**Current release: [0.1.8](../../releases)** ([notes](docs/ufc3/releases/0.1.8.md)). What's new
since 0.1.6:

- **0.1.8:**
  - the Career menu no longer drops to 10–30 FPS after winning the 2nd belt;
  - a controller can disconnect and reconnect without crashing the game;
  - blood and sweat are fixed with `BB_WRITE_WINDOW=2`;
  - old cached textures are freed, so FPS should no longer go down over long sessions.
- **0.1.7:** two Career crashes are fixed (entering Career or loading a save after a title fight,
  and moving the Create Fighter sliders), and the launcher explains when the CPU is too old.

**Playable.** Menus, loading, fights (including the menu's attract-mode fight), sound, controller
and saves. What works today:

- **Career**: it saves, and saves made after a title fight load again. It no longer crashes on
  entry or when you move the Create Fighter sliders or change presets. Changing hair/head
  repeatedly in the fighter editor no longer hangs the GPU, and the menu stays fast after you
  win belts.
- **Blood and sweat** on hits, with the created fighter's skin still correct.
- **PS4 Pro mode** (the game renders 1080p instead of 900p), with a checkbox in the Windows
  launcher (`BB_NEO=1` on Linux).
- **2K / 4K (experimental)**: 2560x1440 and 3840x2160 internal resolution in the launcher (validated
  on Linux only; see *Launcher settings*).
- **90 FPS** in the launcher's FPS limit, next to 60 / 120 / 144 / 240 / Unlimited.
- **Hair and beards** with proper coverage instead of square cards.
- **Created-fighter skin** that survives repeated editing (no black or garbled patches).
- **Shadows** without acne or flicker from the emulated depth format.
- **Career portraits and the post-fight screen**: your fighter shows up on the post-fight menu,
  in the career pause menu and on the Saved Careers cards.
- **Game language** selector in the Windows launcher.
- **Launchers** on both systems (Windows: UFC3.exe; Linux: a GTK launcher), with Offline / Join
  server / Host server.
- **Online between friends** (private hosting, rematch included); see *Online*.

| Scene | Linux · Ryzen 5 5600 + RTX 4070 | Windows · Omen 16 (RTX 3060 Laptop, 6 GB) |
|---|---|---|
| Menus / loading screens | 240 FPS (the game's own cap) | 240 FPS (*Immediate* present mode) |
| Career fight, no FPS limit | — | **60–70 FPS** (0.1.5, write windows on, log folder on a local disk) |
| Fight, no FPS limit | **137–196 FPS** (write windows on) | 66–83 FPS (0.1.4, a Quick Fight without write windows) |
| Fight, 120 FPS limit | steady 120 FPS | — |

- Tested by the maintainer on NVIDIA (Linux and Windows); AMD fixes come from @luscamovi's
  RX 9070, and players have reported on other AMD and NVIDIA cards (RX 7800 XT, RTX 4090…).
- **Write windows** (a faster CPU/GPU memory path, see *Performance*) are **on on both Linux and
  Windows** since 0.1.5: UFC-48 fixed the Windows heap corruption of 0.1.4.

**Known issues**

- Fights on Windows still run below Linux.
- Keep the log folder on a local disk for the best FPS.
- "Clear the shader cache at start" recompiles every shader on each start: leave it off.
- The 0.1.8 fixes (Career menu FPS, controller reconnect, blood/sweat in mode 2, long sessions)
  were checked with tests and code review, not yet by many players: if one does not fix your case,
  please reopen the issue with the new log.
- The 2K/4K internal resolutions are still experimental. If the game crashes with one of them,
  try again without it before you report the crash.
- Moving the window to another monitor *during startup* can crash the NVIDIA driver
  (`Swapchain::Destroy`).
- Some long hairstyles leave a slightly lighter line at the base of the neck.
- Portuguese (Portugal) (`BB_LANGUAGE=7`) is listed but the game has no text for it: pick
  Portuguese (Brazil) (`17`).

## Requirements

- x86-64 CPU with AVX2, BMI1, MOVBE and LZCNT (Intel Haswell / 4th-gen Core or newer, or AMD
  Ryzen). Older CPUs with only AVX (Sandy/Ivy Bridge, Xeon E5 v1/v2) are not supported yet.
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
   session's log goes to `%LOCALAPPDATA%\UFC3Native\logs\`; the launcher's **Open logs folder**
   button opens it. **If the game crashes, send us that session's `.log`** (with **Frame
   statistics** checked if you can reproduce it).

### Launcher settings (God of War III port style)

Since 0.1.5 the launcher has a UFC 3 dark/gold look, with your dump's own artwork in the banner.

| Setting | Values | Notes |
|---|---|---|
| Player name | text (16 ASCII characters) | The name the game shows. |
| Game language | English (US), French, German, Italian, Spanish (Spain / Latin America), Portuguese (Brazil / Portugal), Polish, Russian, Japanese, Chinese (Traditional) | `BB_LANGUAGE`. Portugal has no text in the game: use Brazil. |
| Controls... | per PS4 input | Keyboard key (click, then press it) and gamepad button. Saved to `bbport.ini`. |
| Online play | **Offline** / Join server / Host server | Join: the host's address in *Join address*. Host: pick this PC's address in *Host address* (Rescan, **Copy IP** to share it); the server starts with Play and stops with the game. See *Online*. |
| Game resolution | 720p / 1080p / 2560x1440 (2K, experimental) / 3840x2160 (4K, experimental) | What the game renders. 2K/4K use PS4 Pro mode with a 4K output; in game choose VIDEO → RESOLUTION → *Favor Resolution* (the option only appears in these modes). Validated on Linux only. |
| Display resolution | Same / 1080p / 1440p (2K) / 4K | Window size; the game's frame is scaled to it (use FSR 1.0 + sharpening). Fullscreen uses the monitor's size. |
| PS4 Pro mode | on / **off** | `BB_NEO`: the game renders 1080p instead of 900p, at a lower frame rate. |
| FPS limit | 60 / 90 / 120 / 144 / 240 / **Unlimited** | Saved by value; the game never goes above 240. |
| Present mode | Mailbox / VSync (FIFO) / **Immediate** | On NVIDIA, Mailbox stops at 2× the monitor's refresh rate; Immediate reaches 240 (may tear). |
| Upscaling | Off / AMD FSR 1.0 | For an internal resolution below the monitor's. |
| Sharpness (RCAS) | Off / Low / Medium / High | |
| FPS counter | on / off | Top left. |
| Frame statistics | on / off | Periodic FPS, readback and (on Windows) memory figures in the log; useful in crash reports. |
| Clear the shader cache at start | on / **off** | Recompiles every shader on each start: leave it off. |
| Log folder | a folder (empty: `%LOCALAPPDATA%\UFC3Native\logs`) | Where the run logs and frame/readback CSVs go; `BB_LOG_DIR` wins over it. A network drive works, but a local disk gives the best FPS. |

Environment variables you already have set take precedence over the launcher (for example a
`.bat` with `set BB_WRITE_WINDOW=2` before starting `UFC3.exe`). Since 0.1.7 you no longer need
`BB_WRITE_WINDOW=0` to avoid the Career crashes.

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

There is also a GTK 4 launcher (game folder, player name, language, resolution, Offline / Join /
Host) that needs Python PyGObject and GTK 4: `python3 launcher/ufc3_launcher.py` from the repo,
or `./ufc3.sh` with no arguments in the Linux package (`./ufc3.sh --play /path/to/CUSA06534`
plays from the command line). Logs go to `~/.local/share/UFC3Native/logs/`.

## Online (experimental)

Private hosting between friends: one player runs the server on their own PC, with no public
matchmaking. Either Windows or Linux can host, and the two PCs may run different systems; both
need the same port build and the same game dump/update.

- The host picks **Host server** in the launcher, selects their address, shares it (**Copy IP**)
  and presses **Play**. The friend picks **Join server**, enters that address and presses **Play**.
- Both enter Online → Quick Fight and search at about the same time. **Rematch works.**
- Outside your LAN, use a VPN such as [Tailscale](https://tailscale.com) on both PCs and share the
  host's VPN IP (never `127.0.0.1`).

Step by step: [`docs/ufc3/HOW-TO-PLAY-ONLINE.md`](docs/ufc3/HOW-TO-PLAY-ONLINE.md); ports, logs and
troubleshooting: [`docs/ufc3/ONLINE-HOSTING.md`](docs/ufc3/ONLINE-HOSTING.md).

Advanced: the launchers set `BB_ONLINE_SERVER=<host address>` for the game (without it the
console stays offline). An inherited `BB_ONLINE_SERVER` overrides the launcher's Offline/Join
choice. You can also run the server yourself (`python3 tools/ufc3_server.py --bind 0.0.0.0`)
and have both players use Join server. `BB_NET_LOG=0` silences the network log (on by default in
this mode). Details: [`docs/ufc3/ONLINE_IMPLEMENTATION.md`](docs/ufc3/ONLINE_IMPLEMENTATION.md).

## Building the Windows zip (on Linux)

```bash
bash scripts/build-windows.sh                 # llvm-mingw cross-compile, LTO on
BB_ALLOW_DIRTY=1 bash scripts/package-windows.sh
# -> dist/UFC3Native-windows-x86_64.zip  (UFC3.exe, bin/, embedded Python, scripts/)
```

### Publishing a release

Write the notes in `docs/ufc3/releases/<version>.md`, commit, then:

```bash
bash scripts/release.sh <version>   # tags ufc3-v<version>, builds, packages, pushes, creates the GitHub release
```

## Key environment variables

| Variable | UFC 3 default | What it does |
|---|---|---|
| `BB_FPS_LIMIT` | 0 on Windows (unlimited; the launcher offers 60/90/120/144/240), 120 in `run.sh` | FPS limit (0 = none). |
| `BB_GUEST_FPS_LIMIT` | 0 | FPS limit applied to the game's own frame loop (once per frame in `sceGnmSubmitDone`), instead of holding finished frames in the flip queue as `BB_FPS_LIMIT` does: the next frame reads the pad after the wait, so the limit adds no input latency. With `BB_FRAMES_AHEAD=1` it lowered the GPU wait per frame from 4.7 to 1.9 ms on a GTX 1650. |
| `BB_NEO` | 0 (launcher: "PS4 Pro mode") | PS4 Pro mode: the game renders 1080p instead of 900p (about 80 FPS in fights on an RTX 4070). |
| `BB_PRESENT_MODE` | Immediate (Windows) | `Mailbox`, `Fifo` or `Immediate`. |
| `BB_VBLANK_HZ` / `BB_FLIP_RATE` | 480 / 0 | The game asks for 30 Hz; with flip rate 0 and a high virtual vblank it runs at the real rate (the simulation follows real frame time). |
| `BB_READBACKS` / `BB_READBACK_LINEAR` | 2 / 1 | Precise reads of GPU-written memory (without them the fight renders black, as UFC 4 does in shadPS4). |
| `BB_WRITE_STEP` | 1 | CPU writes next to a few GPU-written bytes are single-stepped instead of waiting for the GPU; write windows need it. Validated on Windows (UFC-48 coherent windows) with correct created/career skin. |
| `BB_WRITE_WINDOW` | 1 (`run.sh` and the Windows launcher, since 0.1.5) | Pages shared by CPU and GPU data stay open for the CPU until the game's next submission; then only what changed is uploaded (see *Performance*). The 0.1.4 Windows heap corruption is fixed by UFC-48's coherent windows (validated on an RTX 3060: second fight and contract without a crash). `BB_WRITE_WINDOW=0` turns it off. `BB_WRITE_WINDOW=2` opens a window only when the faulting write's exact bytes are known (a guest `memcpy`/`memmove`/`memset`, or a guest store decoded as one plain store) and they do not cover GPU bytes; any other write is handled as with 0 (single step or readback). It removes the per-frame readbacks of the `Map(WRITE_DISCARD)` + `memcpy` at guest `+0x431c4f` and the stores at `+0x4219b8` (see `docs/ufc3/WINDOWS-EXACT-WRITE-WINDOWS.md`). Since 0.1.8 mode 2 no longer overwrites GPU data next to a CPU write, which broke blood and sweat. |
| `BB_PREUPLOAD` | 1 | Background upload of rewritten game memory into VRAM (1 = resident memory, 2 = all of it). Restores blood/sweat on hits (UFC-49); validated on Linux and Windows. A user-set value overrides the default. |
| `BB_ALPHA_TO_MASK` | on | Hair/beard coverage (`DB_ALPHA_TO_MASK` emulated as a shader dither). `0` turns it off; `coverage` uses Vulkan alpha-to-coverage on MSAA targets; `colourmap` limits it to draws sampling the hair colour map (Create Fighter only). |
| `SHADPS4_BC_MODE` | 3 | GPU-compressed textures go through memory (created-fighter skin). `0` restores the older compressed-view/copy behaviour, for comparison. |
| `BB_T4_PRESERVE_ALIAS` | 1 | Keeps the portrait the game draws into a larger render target when it re-reads it at the portrait's size (post-fight fighter, career portraits). `0` disables it. |
| `BB_LDS_ORDER` | 1 | Subgroup memory barriers around shared-memory (LDS) stores in translated shaders, which the GCN code expects (octagon floor grain on AMD). `0` turns them off; clear the shader cache when changing it. |
| `BB_GPU_WRITE_TWINS` | 0 | Incompatible with precise readbacks (deadlock). |
| `BB_INTERNAL_WIDTH/HEIGHT`, `BB_FSR1`, `BB_RCAS` | from the launcher | Internal resolution and upscaling. |
| `BB_WINDOW_SIZE` | from the launcher | Window size (`2560x1440`, `3840x2160`…); the game's frame is scaled to it. |
| `BB_SKIP_MOVIES` | 1 | The EA intro and the first-start video play a short black clip (`share/skip_movie.mkv`). |
| `BB_USER_NAME` | `Player 1` | The PS4 user name the game shows (16 bytes). |
| `BB_LANGUAGE` | 1 (launcher: "Game language") | The PS4 system language: 1 English (US), 2 French, 3 Spanish (Spain), 4 German, 5 Italian, 0 Japanese, 7 Portuguese (Portugal; the game has no text for it), 8 Russian, 10 Chinese (Traditional), 16 Polish, 17 Portuguese (Brazil), 20 Spanish (Latin America). |
| `BB_FRAME_STATS` | 1 in `run.sh` | Periodic statistics in the log (FPS, µs per draw, readbacks, write sites…). |
| `BB_UI_BACKPRESSURE` | 1 | A UI scene built 8 times since the last frame waits for the next frame, up to 250 ms. This fixes the Career crash in the "UI Update" thread (0.1.7). `0` turns it off. |
| `BB_APPEARANCE_BACKPRESSURE` | 1 | When the fighter appearance pool is full, the game waits up to 250 ms for an entry to be freed. This fixes the Create Fighter crash (0.1.7). `0` turns it off. |
| `BB_DEPTH_AS_COLOR_REUSE` | 1 | A shadow map read as color is converted only again after it changes, instead of at every draw. This fixes the slow Career menu after winning the 2nd belt (0.1.8). `0` converts at every draw, as before. |
| `BB_GC_STALE_SECONDS` | on | Frees cached textures that the game overwrote and that were not used for 60 s, even when memory use is low (0.1.8). `N` (1–63) changes the number of seconds; `0` turns it off. |
| `BB_GROWTH_SECONDS` | 60 | How often the log prints a `Growth` line with cache sizes, memory and FPS. `0` turns it off. |

There are also purely diagnostic switches (`*_TRACE`, `*_SKIP`, `*_DRAWS`…); they are described
in the [release notes](docs/ufc3/releases/).

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
   On by default (write windows need it); `BB_WRITE_STEP=0` turns it off.
4. **`BB_WRITE_WINDOW`** (the big fight improvement). A new per-site counter ("Readback sites" in
   the log) showed that almost every wait came from the EA engine: its dynamic buffers (a lock
   with discard at `0x64b50` → `0xf4120`, e.g. the fighters' 48-byte bone matrices) share pages
   with small shader outputs (e.g. a 408-byte binding at `0x1001338ee0`). Every CPU write forced
   a wait for the GPU and a page download. Now the page stays open for the CPU until the next
   `SubmitGfx`/`SubmitAsc`; it is then protected again, compared with the copy taken when it was
   opened, and only the changed words are uploaded (skipping the GPU's). Result on Linux:
   readbacks 380 → 120 per second and fights from **105–113 to roughly 140–190 FPS**
   on an RTX 4070 (the detailed run recorded 137–196 FPS uncapped).
   In 0.1.4 the windows corrupted the game's heap on Windows (crashes loading the second fight
   or signing the career contract), so that launcher turned them off. UFC-48 made them
   coherent; since 0.1.5 **`BB_WRITE_WINDOW=1` is the default on Linux and Windows**
   (validated on an RTX 3060: second fight and contract without a crash). An explicit value
   (`BB_WRITE_WINDOW=0`) wins on either platform.

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
- `BB_WRITE_STORE_DIAG=1` (with `BB_WRITE_WINDOW=2`) — logs, once per site, the game's stores
  whose exact bytes the port cannot tell.

## Credits

- [SacredFire](https://github.com/T3CHNOLOG1C/SacredFire) — the base runtime, renderer and Windows
  packaging (its original README is [`README.SacredFire.md`](README.SacredFire.md)).
- [bbport](https://github.com/deadinside28/bloodborne_pc) — the native port model.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) — renderer and shader recompiler.
- The God of War III Windows port and *eltutz*'s work, as references for Windows tuning (timers,
  priorities, GPU readback model).
- **[@luscamovi](https://x.com/luscamovi)** (on X) — the AMD (RX 9070) patches, PS4 Pro mode and
  the career save fix (also the AMD octagon floor grain, the 90 FPS option and the launcher's
  Pro checkbox).
- **mancelli** and their [shadPS4 fork](https://github.com/mancelli/shadPS4/releases/tag/ufc4-intel-arc-2026-10-14)
  — hair (`DB_ALPHA_TO_MASK`), created-fighter skin, depth bias and NaN fixes.
- Upstream shadPS4 authors whose PRs were backported: TheTurtle, Claudio Godoi, kiwakiwaa,
  artemkaVG, DanielSvoboda and Chris Chreece (original commits in [`gpu/VENDOR.txt`](gpu/VENDOR.txt)).
- **HeuZ** — Windows RTX 4090 logs that located the write-window crash; **pvict** (GitHub) —
  Portuguese testing with `BB_LANGUAGE` that motivated the Windows language selector.
  Thanks to everyone who sent Windows logs and reports.
- **@SinLeeke** — exact write windows (`BB_WRITE_WINDOW=2`) and the guest frame limit (PR #13).
- Players whose reports and logs led to the 0.1.7–0.1.8 fixes: **@DieYoung0800**,
  **@Cheburechek02**, **@nomadsdad**, **@ThallesCosta-dev**, **@VHXXIII**, **@waghrudraksh**,
  **@bigozerkibigozerki-jpg**, **@paulkresnik**, **@Beiudo** and **@SercanDmr**.
- **[Flawlessix](https://www.reddit.com/user/Flawlessix/)** (Reddit) — testing on AMD.

- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [minimp3](https://github.com/lieff/minimp3),
  [FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR), Dear ImGui.

Detailed release notes and patch credits: [`0.1.3`](docs/ufc3/releases/0.1.3.md),
[`0.1.4`](docs/ufc3/releases/0.1.4.md), [`0.1.5`](docs/ufc3/releases/0.1.5.md),
[`0.1.6`](docs/ufc3/releases/0.1.6.md), [`0.1.7`](docs/ufc3/releases/0.1.7.md) and
[`0.1.8`](docs/ufc3/releases/0.1.8.md).

## License

GPL-2.0-or-later, like bbport, SacredFire and shadPS4 (see [`LICENSE`](LICENSE)).
