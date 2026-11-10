#!/bin/sh
# Download one GitHub release asset, retrying and falling back to the API.
#
#   tools/fetch_release_asset.sh OWNER/REPO TAG NAME OUTPUT [SHA256]
#
# github.com's /releases/download/ URLs sometimes answer 504 for one asset for
# minutes at a time while the same file downloads fine through the API, so a CI
# run with several such downloads used to fail on one of them almost every
# time. This tries the normal URL a few times, then the API's asset endpoint
# (authenticated with GH_TOKEN or GITHUB_TOKEN when set, for its rate limit),
# with a pause between rounds. With SHA256 the file is checked, and a mismatch
# fails at once rather than being retried. Uses POSIX sh, awk and curl, so it runs
# in the minimal containers CI builds in.
set -eu
[ $# -ge 4 ] || { echo "usage: $0 OWNER/REPO TAG NAME OUTPUT [SHA256]" >&2; exit 2; }
repo=$1 tag=$2 name=$3 out=$4 sha=${5:-}
token=${GH_TOKEN:-${GITHUB_TOKEN:-}}
tmp="$out.part"

api_get() {
    if [ -n "$token" ]; then
        curl -fsSL --max-time 300 -H "Authorization: Bearer $token" "$@"
    else
        curl -fsSL --max-time 300 "$@"
    fi
}

# The asset's API id, from the release's JSON (no jq in the build images).
# Keep object boundaries and quoted strings intact, including nested uploader
# objects. Whitespace, property order and regex characters in names do not matter.
asset_id() {
    api_get -H "Accept: application/vnd.github+json" \
        "https://api.github.com/repos/$repo/releases/tags/$tag" |
        ASSET_NAME="$name" awk -f "$(dirname "$0")/release_asset_id.awk"
}

fetch() {
    if curl -fsSL --max-time 300 -o "$tmp" "https://github.com/$repo/releases/download/$tag/$name"; then
        return 0
    fi
    id=$(asset_id || true)
    [ -n "$id" ] || return 1
    api_get -H "Accept: application/octet-stream" -o "$tmp" \
        "https://api.github.com/repos/$repo/releases/assets/$id"
}

round=1
while :; do
    if fetch; then break; fi
    if [ "$round" -ge 5 ]; then
        echo "fetch_release_asset: $repo $tag $name failed after $round rounds" >&2
        rm -f "$tmp"
        exit 1
    fi
    echo "fetch_release_asset: $name round $round failed, retrying in $((round * 15))s" >&2
    sleep $((round * 15))
    round=$((round + 1))
done

if [ -n "$sha" ]; then
    echo "$sha  $tmp" | sha256sum -c - >/dev/null || {
        echo "fetch_release_asset: $name does not match its pinned sha256" >&2
        rm -f "$tmp"
        exit 1
    }
fi
mv "$tmp" "$out"
