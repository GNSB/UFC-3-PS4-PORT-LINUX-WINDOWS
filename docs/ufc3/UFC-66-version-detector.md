# UFC-66: compiled build identities

`build.sh` and `scripts/build-windows.sh` resolve the revision once, before compilation,
then pass it to the game, GPU shim and Windows launcher and write the same value to
`VERSION`. Untagged commits and dirty worktrees are supported. CMake builds made
without the scripts default to `unknown`.

The game supports `--version`: it prints only its compiled version and exits before
host memory reservation, CPU checks, GPU initialization or guest loading. Normal
startup reports game and GPU versions. On Linux, a GPU library predating the new
export reports `unknown` and triggers the warning.

The Windows launcher reads the game's NUL-terminated `BB_GAME_BUILD_VERSION=` marker
from the exe, without starting it. Missing, legacy or malformed identities report
`unknown` and trigger the warning. Both the visible log and the run/launcher log
include launcher, VERSION-file and game identities. The run header is emitted before
preparation, so even failed game preparation retains these identities.

Linux `run.sh` compares the VERSION file with the actual executable's `--version`.
The packaged `ufc3.sh` supplies its absolute `bin/sacredfire` path and delegates the
comparison to `run.sh`, where log capture is active. `BB_PROBE` wrappers do not replace
the executable used for the identity query.

Fault headers include the compiled game version on both platforms; `fault_extra`
repeats it alongside the additional game diagnostics.

## Validation

- `ufc3-build --version-test`: short comparison/warning and exe-marker parsing tests,
  game `--version` check, and embedded game/GPU identity checks. Does not run the guest.
- `ufc3-build --test`: includes those checks plus the existing full suite.
- `ufc3-build --windows-runtime`: builds the Windows GPU static library, game and launcher
  through `scripts/build-windows.sh`, under the machine-wide build lock.
- On Windows, copy a release to a disposable folder, replace its VERSION text with
  another version, and open SacredFire.exe. The visible log and launcher.log must warn.
  Starting a run must repeat the warning in the run log. A matching release must not warn.

## Example logs

Matching Windows release:

```text
SacredFire ufc3-v0.1.8 (Windows)
Version: launcher ufc3-v0.1.8, VERSION file ufc3-v0.1.8, game ufc3-v0.1.8 (Windows)
Version: game ufc3-v0.1.8, GPU library ufc3-v0.1.8 (Windows)
```

Mixed Windows release:

```text
SacredFire ufc3-v0.1.8 (Windows)
Version: launcher ufc3-v0.1.8, VERSION file ufc3-v0.1.8, game ufc3-v0.1.7 (Windows)
VERSION MISMATCH: components differ or are unknown — extract the release zip into a new, empty folder
Version: game ufc3-v0.1.7, GPU library ufc3-v0.1.7 (Windows)
```

Mixed Linux shared library:

```text
SacredFire ufc3-v0.1.8
Version: VERSION file ufc3-v0.1.8, game ufc3-v0.1.8 (Linux)
Version: game ufc3-v0.1.8, GPU library ufc3-v0.1.7 (Linux)
VERSION MISMATCH: components differ or are unknown — extract the release zip into a new, empty folder
```
