#!/usr/bin/env bash
# Creates the GitHub release for TAG with the given package files attached,
# using the CHANGELOG.md section for that version as the release notes. If the
# release already exists, the files are uploaded to it, replacing any with the
# same name. Run by .github/workflows/release.yml; needs gh with a token that
# can write contents.
#
#   bash packaging/publish_release.sh vX.Y.Z FILE...
set -euo pipefail

tag="${1:?usage: publish_release.sh vX.Y.Z FILE...}"
shift
(( $# > 0 )) || { echo "publish_release: no files to attach" >&2; exit 64; }
repo="$(cd "$(dirname "$0")/.." && pwd)"
version="${tag#v}"
source_version="$(sed -n 's/.*DIAMOND_VERSION\[\] = "\(.*\)";/\1/p' "$repo/src/main.c")"
if [[ "$version" != "$source_version" ]]; then
    echo "publish_release: tag $tag does not match src/main.c version $source_version" >&2
    exit 1
fi

# The section runs from "## VERSION" (optionally followed by " — DATE") to the
# next "## " heading.
notes="$(awk -v version="$version" '
    index($0, "## " version) == 1 && (length($0) == length(version) + 3 || substr($0, length(version) + 4, 1) == " ") { found = 1; next }
    found && /^## / { exit }
    found { print }
' "$repo/CHANGELOG.md")"
if [[ -z "${notes//[[:space:]]/}" ]]; then
    echo "publish_release: CHANGELOG.md has no section for $version" >&2
    exit 1
fi

if gh release view "$tag" >/dev/null 2>&1; then
    gh release upload "$tag" "$@" --clobber
else
    gh release create "$tag" "$@" --verify-tag --title "Diamond $version" --notes "$notes"
fi
