#!/usr/bin/env bash
# UFC 3: tag, build, package and publish a GitHub release of the Windows build.
# Usage: scripts/release.sh <version>   (e.g. 0.1.0 -> tag ufc3-v0.1.0)
# The release notes come from docs/ufc3/releases/<version>.md; the checkout must be committed.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
version=${1:?usage: scripts/release.sh <version>}
tag=ufc3-v$version
notes=docs/ufc3/releases/$version.md
[[ -f $notes ]] || { echo "release: write $notes first" >&2; exit 1; }
git update-index -q --refresh
git diff-index --quiet --ignore-submodules=dirty HEAD -- || { echo "release: commit the changes first" >&2; exit 1; }
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then echo "release: $tag already exists" >&2; exit 1; fi
repo=$(git remote get-url origin | sed -E 's#^https://github.com/##; s#^git@github.com:##; s#\.git$##')

git tag -a "$tag" -m "UFC 3 native port $version"
trap 'echo "release: failed; removing the local tag $tag" >&2; git tag -d "$tag" >/dev/null' ERR
bash scripts/build-windows.sh
bash scripts/package-windows.sh
asset=dist/UFC3Native-$tag-windows-x86_64.zip
cp dist/UFC3Native-windows-x86_64.zip "$asset"
(cd dist && sha256sum "$(basename "$asset")" > "$(basename "$asset").sha256")
git push origin HEAD:main "$tag"
trap - ERR
gh release create "$tag" "$asset" "$asset.sha256" --repo "$repo" \
    --title "UFC 3 native port $version" --notes-file "$notes"
echo "release: https://github.com/$repo/releases/tag/$tag"
