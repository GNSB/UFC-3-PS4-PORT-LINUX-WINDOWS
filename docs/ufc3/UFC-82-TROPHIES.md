# UFC-82: local trophies (issue #45)

No PSN, no network. `src/runtime_trophy.c` keeps them; `src/runtime_services.c` holds the
`sceNpTrophy*` entries.

- **Saved:** every unlock goes to `trophies.json` in the user folder (`--user`, the same folder as
  saves and logs), written to `trophies.json.tmp` and renamed over the old file:

  ```json
  {
    "version": 1,
    "unlocked": [
      {"id": 3, "date": "2026-10-20T16:00:20Z", "unix": 1792512020}
    ]
  }
  ```

  Deleting the file locks every trophy again.
- **The game's list:** `sceNpTrophyGetGameInfo`/`GetTrophyInfo` report the count (the
  `TROPnnn.PNG` icons in `sce_sys/trophy/trophy00.trp`), the unlocked ones, progress and unlock
  dates (layouts from shadPS4's `np_trophy.h`).
- **Toast:** about 4 s in the top-right corner with the trophy's icon and "Trophy unlocked: #N".
  It can be turned off with `trophy_notifications=0` in `bbport.ini`, or "Trophy notifications" in the in-game menu.
- **Names (optional):** the names, grades and title are in `TROP.ESFM`, which is encrypted. With
  the user's own trophy key (`trophy_key=<32 hex digits>` in `bbport.ini`, or shadPS4's `keys.json`
  copied into the user folder) and `sce_sys/npbind.dat` (the NP communication id), the toast shows
  the name, and the platinum unlocks once all the other trophies are. No key is distributed.
- An unlock of a trophy that is already unlocked answers OK, as it did before UFC-82, and shows no
  toast. A real PS4 answers `ALREADY_UNLOCKED`.

Test: `tests/test_trophy.c` (in `build.sh --test`) builds a synthetic TRP; no game data.
