# UFC 3 native port — Windows

This archive runs **EA SPORTS UFC 3** (PlayStation 4, `CUSA06534`, update **1.14**) as a native
Windows x86-64 program. It was built on Linux from the source at the commit in `VERSION`
(https://github.com/GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS) and **contains no game files**: you need
your own dump of the game and its 1.14 update.

## Requirements

- 64-bit Windows 10 (1903 or newer) or Windows 11.
- A GPU with a Vulkan 1.3 driver (tested: NVIDIA RTX 3060 Laptop; AMD fixes tested on an
  RX 9070). 6 GB of VRAM or more is recommended.
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
- **Game language** — the PS4 system language the game reads (its text and menus): English (US),
  French, German, Italian, Spanish (Spain), Spanish (Latin America), Portuguese (Brazil),
  Portuguese (Portugal), Polish, Russian, Japanese, Chinese (Traditional). The game ships no
  European Portuguese text: for Portuguese pick **Portuguese (Brazil)**. A `BB_LANGUAGE` set
  before the launcher (e.g. `set BB_LANGUAGE=17` in a `.bat`) wins over this choice.
- **Controls...** — keyboard keys (click, then press the key) and gamepad buttons for every PS4
  input. The keyboard and a gamepad work together.
- **Game resolution** — 720p / 1080p, what the game renders.
- **Display resolution** — the window size: 1080p, 1440p (2K) or 4K. The game's 1080p frame is
  scaled to it; use **Upscaling = AMD FSR 1.0** and some **Sharpening** for a sharp image. In
  fullscreen the monitor's size is used.
- **PS4 Pro mode** — the game renders 1080p instead of 900p (sharper, lower frame rate).
- **Skip the intro videos** — the EA logo and the first-start video (on by default).
- **Present mode** — *Immediate* reaches the game's 240 FPS cap (may tear); *Mailbox* is capped
  at twice the refresh rate on NVIDIA; *VSync* follows the monitor.
- **FPS limit** — 60 / 90 / 120 / 144 / 240 / Unlimited.
- In game: **Insert** or **L3+R3** opens the overlay menu.

## Problems

If the game crashes, send the newest `.log` from `%LOCALAPPDATA%\UFC3Native\logs` (the
launcher's **Open logs folder**) with a short description; checking **Frame statistics** first
makes the log more useful.

Write windows (a faster memory path) are off on Windows because they corrupted the game's heap
(crashes loading the second fight or signing the career contract). `set BB_WRITE_WINDOW=1` in a
`.bat` before starting `UFC3.exe` turns them back on, at the risk of those crashes.

If the launcher's log says a program "could not start" (Windows error 0xC0000022, access
denied): allow the file in your antivirus (Windows Security > Protection history), extract the
archive outside OneDrive (e.g. `C:\Games\UFC3Native`), or unblock the zip (Properties >
Unblock) and extract it again.

This project is not affiliated with or endorsed by Electronic Arts, Sony Interactive
Entertainment or the UFC. GPL-2.0-or-later; third-party licences are in `licenses\`.
