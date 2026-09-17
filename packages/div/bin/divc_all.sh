#!/usr/bin/env bash
set -euo pipefail

# Recursively compile every *.html.div template below one source directory.
# Generated files use divc.di's ordinary per-directory .cache convention.
diamond="${DIAMOND_BIN:-diamond}"
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -ne 1 ]]; then
  echo "usage: divc_all.sh <template-directory>" >&2
  exit 1
fi

source_dir="$1"
if [[ ! -d "$source_dir" ]]; then
  echo "divc_all.sh: not a directory: $source_dir" >&2
  exit 1
fi

# Refuse a filesystem root: the clean rebuild below is intentionally scoped
# to a template tree, never an entire volume.
resolved_source="$(cd -- "$source_dir" && pwd -P)"
if [[ "$resolved_source" == "/" ]]; then
  echo "divc_all.sh: refusing to use / as a template directory" >&2
  exit 1
fi

find "$resolved_source" -type d -name .cache -prune -exec rm -rf -- {} +

count=0
while IFS= read -r -d '' source; do
  # Passed as divc.di's own name_path (its third argument, with the
  # second left empty so auto output-path derivation still applies) --
  # this file's path relative to $resolved_source, e.g. "skins/show.
  # html.div" for a file under a "skins" subdirectory -- so its
  # generated function name is qualified by directory instead of
  # colliding with another file sharing its basename elsewhere in the
  # tree. A file directly under $resolved_source has no "/" in this
  # relative path, so its generated name is unaffected either way.
  relative="${source#"$resolved_source"/}"
  "$diamond" "$script_dir/divc.di" "$source" "" "$relative"
  count=$((count + 1))
done < <(find "$resolved_source" -type f -name '*.html.div' -print0 | sort -z)

echo "compiled $count templates under $resolved_source"
