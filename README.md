# PORTHEON — Ports & Mods

**PORTHEON** is a launcher for the PC ports of **EA SPORTS UFC 1, 2, 3, 4 (PS4), UFC 5 (PS5) and UFC Undisputed 3 (Xbox 360)**, created and maintained by **Marcelo Miyake (XAndromedaXR)**. It finds the game, reads your PC and picks the best settings for each UFC by itself.

> **No game files are included.** You need your own copy of each game.
> This project is not affiliated with or endorsed by Electronic Arts, THQ, Sony Interactive Entertainment, Microsoft or the UFC. *EA SPORTS UFC*, *UFC Undisputed*, *PlayStation* and *Xbox* are trademarks of their owners and are named only to identify the games.

- 📥 **Download:** [latest release](../../releases/latest)
- 💬 **Discord:** https://discord.gg/2qB2NcgjD
- 📺 **YouTube:** https://www.youtube.com/watch?v=pjO5Du3rbJQ
- 💚 **Support the project (early access to UFC 6):** https://apoia.se/portheon

*(Versão em português logo abaixo.)*

---

## Games

| Game | Platform | What you need | Port |
|---|---|---|---|
| EA SPORTS UFC (2014) | PS4 | `CUSA00222`, update 1.08 | native PC port |
| EA SPORTS UFC 2 | PS4 | `CUSA01968`, update 1.13 | native PC port |
| EA SPORTS UFC 3 | PS4 | `CUSA06534`, update 1.14 | native PC port |
| EA SPORTS UFC 4 | PS4 | `CUSA14209` or `CUSA14204`, update 28.00 | native PC port |
| EA SPORTS UFC 5 | PS5 | `PPSA03541` (tested with v01.031) | PC port (engine `UFC 5.exe`, KytyPS5 base) |
| UFC Undisputed 3 | Xbox 360 | the UFC Undisputed Reborn 1.1 folder (with `Xenia Canary`) | PC port (Xenia Canary of the Reborn) |

## How to install

1. Download `PORTHEON-x.x-Windows-x64.zip` from the [latest release](../../releases/latest).
2. Extract it into a **new, empty folder** (never over an older version).
3. Open `PORTHEON.exe`. On Linux / Steam Deck, run `portheon.sh` (experimental).
4. Pick a game on the left, click **Browse** and choose the game folder.
5. Click **PLAY**.

On 3.2.2 or newer, PORTHEON updates itself when you open it.

## What PORTHEON does

- One window for every UFC: video, gameplay and controller settings written straight into each port.
-

## Credits and license

The EA SPORTS UFC 3 native port that PORTHEON is built on was created and is maintained by
**Gregori Sobrevía ([GNSB](https://github.com/GNSB))**:
https://github.com/GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS

It is based on [SacredFire](https://github.com/T3CHNOLOG1C/SacredFire),
[bbport](https://github.com/deadinside28/bloodborne_pc) and a renderer derived from
[shadPS4](https://github.com/shadps4-emu/shadPS4). Code contributors to the UFC 3 port and the
upstream copyright notices are listed in [`AUTHORS`](AUTHORS).

All code is GPL-2.0-or-later (see [`LICENSE`](LICENSE)).
