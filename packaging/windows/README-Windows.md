# UFC 3 native port — Windows

This archive runs **EA SPORTS UFC 3** (PlayStation 4, `CUSA06534`, update **1.14**) as a native
Windows x86-64 program. It was built on Linux from the source at the commit in `VERSION`
(https://github.com/GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS) and **contains no game files**: you need
your own dump of the game and its 1.14 update.

## Requirements

- 64-bit Windows 10 (1903 or newer) or Windows 11.
- A GPU with a Vulkan 1.3 driver (tested: NVIDIA RTX 3060 Laptop; AMD is expected to work but
  is untested with UFC 3). 6 GB of VRAM or more is recommended.
- A CPU with AVX.
- The game: the base folder `CUSA06534` and the 1.14 update next to it, e.g.
  `PS4\CUSA06534` and `PS4\CUSA06534-patch` (also `-PATCH`, `-update`, `_patch`). They are
  merged automatically into `CUSA06534-merged` with hard links (no extra space on NTFS).

## Playing

1. Extract the archive anywhere (not inside the game folder).
2. Run **`UFC3.exe`**, pick the `CUSA06534` folder with **Browse...**, press **Play**.
3. The first start prepares the game for a few seconds. Data, saves and logs live in
   `%LOCALAPPDATA%\UFC3Native`.

### Launcher settings

- **Player name** — the name the game shows (16 characters, ASCII).
- **Controls...** — keyboard keys (click, then press the key) and gamepad buttons for every PS4
  input. The keyboard and a gamepad work together.
- **Game resolution** — 720p / 1080p, what the game renders.
- **Display resolution** — the window size: 1080p, 1440p (2K) or 4K. The game's 1080p frame is
  scaled to it; use **Upscaling = AMD FSR 1.0** and some **Sharpening** for a sharp image. In
  fullscreen the monitor's size is used.
- **Present mode** — *Immediate* reaches the game's 240 FPS cap (may tear); *Mailbox* is capped
  at twice the refresh rate on NVIDIA; *VSync* follows the monitor.
- **FPS limit** — 60 / 120 / 144 / 240 / Unlimited.
- In game: **Insert** or **L3+R3** opens the overlay menu.

## Problems

Send the newest log from `%LOCALAPPDATA%\UFC3Native\logs` (the launcher's **Open logs folder**)
with a short description. Known issues: noise on the octagon floor; the first-run tutorial
videos cannot be skipped.

This project is not affiliated with or endorsed by Electronic Arts, Sony Interactive
Entertainment or the UFC. GPL-2.0-or-later; third-party licences are in `licenses\`.
