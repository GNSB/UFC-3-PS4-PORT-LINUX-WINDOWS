# SacredFire on Windows

SacredFire runs *Crash Team Racing: Nitro-Fueled* (PlayStation 4, CUSA13795, update 1.21) as a
native Windows x86-64 program. This archive was built on Linux from the SacredFire source at the
commit in `VERSION`; it contains no game files. You need your own dump of the game with the
1.21 update merged in (copy the dumped update over the base game, replacing files, eboot.bin
and sce_sys too).

**Status:** this Windows build has been produced and inspected on Linux. The Windows acceptance
tests (title screen, a race, saving, restart) are recorded in the project's verification
report (`docs/sacredfire/13-windows-port-report.md`); read it before expecting gameplay. If
the report says native Windows runs are BLOCKED or NOT RUN, you are the first tester: please
send the log (see below).

## Requirements

- 64-bit Windows 10 version 1903 or newer, or Windows 11 (the build uses the UTF-8 process
  code page and the placeholder memory APIs of those versions).
- A Vulkan 1.3 graphics driver with geometry shaders (every current AMD, NVIDIA and Intel
  driver; `vulkan-1.dll` comes with the driver, not with this archive).
- A CPU with AVX, BMI1, MOVBE, LZCNT and POPCNT (Intel Haswell or AMD Ryzen and newer): the
  game's own code uses them.
- About 6 GB of free memory and a page file: the game's 5 GB of console memory lives in a
  pagefile-backed section.

Nothing has to be installed: no Python, no runtime package, no compiler.

## Start

1. Unpack the archive anywhere (a folder without special permissions is best, e.g.
   `C:\Games\SacredFire`).
2. Run `SacredFire.exe`: the launcher window. *Browse...* to the game folder (the one with
   `eboot.bin`), pick the controller for port 1 if you have several, tick the patches and
   switches you want, *Play*. Everything is remembered in
   `%LOCALAPPDATA%\SacredFire\sacredfire.ini` (the patches in `bbport.ini`, shared with the
   in-game menu).
3. The log of the run scrolls in the window; the game window opens when the renderer is ready.
   The first start compiles shaders as it goes (slower); later starts preload them. *Stop* ends
   the game; closing the launcher asks first while the game runs.

*Add to Steam* writes a non-Steam shortcut ("SacredFire", `SacredFire.exe --play`) for every
Steam user of this machine; close Steam first (it overwrites its shortcuts when it exits), then
start Steam to see it. Without the window (that shortcut, a script): `SacredFire.exe --play`
uses the saved settings; `--game "D:\Games\CUSA13795"` names the folder. Further arguments go to the game
(`--timeout 120` for a watchdog, `--list-imports`). Environment variables of the Linux
`run.sh` apply on top of the window's switches: `BB_NEO=1` (PS4 Pro mode), `BB_FPS=60`,
`BB_VK_VALIDATION=1`, `BB_CLEAR_SHADER_CACHE=1`, `BB_DATA_DIR`, `BB_USER_DIR`,
`BB_SKIP_GAME_CHECK=1`. Started from a console, the output also goes there.

## Where things are

| what | where |
|---|---|
| saves | `%LOCALAPPDATA%\SacredFire\user\savedata\1\CUSA13795\...` |
| shader and pipeline cache | `%LOCALAPPDATA%\SacredFire\user\cache\CUSA13795` |
| settings (in-game menu, patches, controller ports) | `%LOCALAPPDATA%\SacredFire\bbport.ini` |
| generated image of the game (from your eboot; delete to regenerate) | `%LOCALAPPDATA%\SacredFire\out` |
| the program | this folder: `SacredFire.exe`, `bin\sacredfire-game.exe`, `python\`, `scripts\`, `patches\` |

`BB_DATA_DIR=.` (set before starting) keeps everything beside `SacredFire.exe` (portable use).
Saves are written as a copy that replaces the old file in one step, so a crash mid-save keeps
the previous save.

## Controllers and the in-game menu

SDL3 gamepads (XInput, DualShock, DualSense, Switch Pro); the keyboard works beside a gamepad
(WASD left stick, arrows right stick, Space Cross, LShift Circle, E Square, Q Triangle, 1/3
L1/R1, R/F L2/R2, Enter Options). The in-game menu (Insert, or L3+R3) switches the 60 FPS and
motion blur patches, assigns controller ports for split screen and the FPS counter; patches
apply after a restart.

## Logs and problems

Every run writes its whole output to `%LOCALAPPDATA%\SacredFire\logs\<date>_<time>.log`
(the first line in the window names the file; *Open logs folder* shows them), the window
shows the same lines, and the last line gives the game's exit code. For a report, attach
that log and name your GPU and driver version. A line starting with `STOP:` or `Guest fault`/`Host fault` names the failure; the
lines after it are the call chain. `SacredFire issue #2` in the log is a known problem (see
the main README): a fence label of the port landing in a command buffer the game has rebuilt.
Since the 2026-10-12 build such a write is caught (`Late label guarded: ...` with the writer's
call chain) and the game goes on; please still send that log, it names the path to fix.

Known differences from the Linux build: the dma-buf memory model (`BB_PC_MODEL`), userfaultfd
tracking (`BB_UFFD`), the single-step heap diagnostics (`BB_FREE_CHECK`, `BB_LABEL_TRAP`,
`BB_HEAP_HOOKS`), MangoHud, PGO builds and mod folders (symlink overlays) are not available on
Windows and say so in the log when asked for. AvPlayer video playback is not built: CTR does
not use it.

## Licence

GPL-2.0-or-later; see `LICENSE` and `licenses/THIRD-PARTY.md` for the components in this
archive and where their source is. Not affiliated with Sony Interactive Entertainment,
Activision or Beenox.
