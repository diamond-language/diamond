#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
mkdir "$scratch/bin"
cp "$repo/tests/fixtures/release_asset_curl.sh" "$scratch/bin/curl"
chmod +x "$scratch/bin/curl"
export PATH="$scratch/bin:$PATH" ASSET_TEST_DIR="$scratch"
unset GH_TOKEN GITHUB_TOKEN

for layout in formatted compact; do
    if [ "$layout" = formatted ]; then
        cp "$repo/tests/fixtures/release_assets.json" "$scratch/release.json"
    else
        tr -d '\n' < "$repo/tests/fixtures/release_assets.json" > "$scratch/release.json"
    fi
    sh "$repo/tools/fetch_release_asset.sh" example/project v1 'wanted+[1].tar.gz' "$scratch/output"
    test "$(cat "$scratch/output")" = 'correct asset'
    test "$(cat "$scratch/requested")" = 'https://api.github.com/repos/example/project/releases/assets/222'
done

# Missing names must not silently select another download.
test -z "$(ASSET_NAME=missing awk -f "$repo/tools/release_asset_id.awk" "$scratch/release.json")"
echo 'release asset fallback tests passed'
