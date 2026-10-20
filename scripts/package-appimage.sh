#!/usr/bin/env bash
# Wrap the existing Linux tar.gz without rebuilding or changing its contents.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."

tool=${APPIMAGETOOL:-appimagetool}
tool=$(command -v -- "$tool") || {
    echo 'package-appimage: appimagetool is required. Install it on PATH, or set APPIMAGETOOL=/absolute/path/to/appimagetool (an executable x86_64 AppImage also works).' >&2
    exit 1
}
# Resolve relative overrides before appimagetool changes directories internally.
tool=$(realpath -- "$tool")
archive=dist/UFC3Native-linux-x86_64.tar.gz
[[ -f $archive ]] || {
    echo "package-appimage: missing $archive. Build with ufc3-build, then run bash scripts/package-linux.sh first (BB_ALLOW_DIRTY=1 for a local test package)." >&2
    exit 1
}

mkdir -p out dist
work=$(mktemp -d "$PWD/out/appimage.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
appdir=$work/UFC3Native.AppDir
mkdir -p "$appdir/usr/lib"
tar -xzf "$archive" -C "$appdir/usr/lib"
payload=$appdir/usr/lib/UFC3Native
for entry in ufc3.sh run.sh launcher/ufc3_launcher.py launcher/ufc3_online.py scripts/prepare.py bin/sacredfire; do
    [[ -f $payload/$entry ]] || { echo "package-appimage: Linux archive is missing $entry" >&2; exit 1; }
done
install -m755 packaging/linux/AppRun "$appdir/AppRun"
install -m644 packaging/linux/UFC3Native.desktop packaging/linux/UFC3Native.svg "$appdir/"
ln -s UFC3Native.svg "$appdir/.DirIcon"

# Python, PyGObject and GTK stay on the host, as in the tar.gz. In particular, do not
# add bin/lib to LD_LIBRARY_PATH here: those libraries belong to the game, not GTK.
# This also lets an AppImage-distributed appimagetool run on builders without FUSE.
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" "$appdir" "$work/UFC3Native-x86_64.AppImage"
install -m755 "$work/UFC3Native-x86_64.AppImage" dist/UFC3Native-x86_64.AppImage
(cd dist && sha256sum UFC3Native-x86_64.AppImage > UFC3Native-x86_64.AppImage.sha256)
echo 'package-appimage: dist/UFC3Native-x86_64.AppImage'
