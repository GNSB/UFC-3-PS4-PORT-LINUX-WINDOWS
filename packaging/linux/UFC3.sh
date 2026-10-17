#!/usr/bin/env bash
# Open UFC 3's graphical launcher from a release archive or source checkout.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
if [[ -f $here/launcher/ufc3_launcher.py ]]; then
    root=$here
else
    root=$(cd -- "$here/../.." && pwd)
fi
exec python3 "$root/launcher/ufc3_launcher.py" "$@"
