#!/bin/sh
set -eu
output=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -o) output=$2; shift 2 ;;
        --max-time|-H) shift 2 ;;
        -*) shift ;;
        *) url=$1; shift ;;
    esac
done
case "$url" in
    https://github.com/*) exit 22 ;;
    */releases/tags/*) cat "$ASSET_TEST_DIR/release.json" ;;
    */releases/assets/222)
        printf '%s\n' "$url" > "$ASSET_TEST_DIR/requested"
        printf 'correct asset\n' > "$output" ;;
    *) echo "unexpected download: $url" >&2; exit 1 ;;
esac
