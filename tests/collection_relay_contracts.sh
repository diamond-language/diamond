#!/usr/bin/env bash
set -euo pipefail

compiler_source="src/compiler.c"
vm_source="src/vm.c"

mapfile -t relay_entries < <(
  sed -n '/static const CollectionRelayContract contracts\[\]/{:a;n;/^    };/q;p;ba}' "$compiler_source" |
    grep -oE '[{]"[^"]+",COLLECTION_RELAY_[A-Z_]+' || true
)

if ((${#relay_entries[@]} == 0)); then
  echo "collection relay audit: compiler contract table is empty" >&2
  exit 1
fi

mapfile -t relay_names < <(
  printf '%s\n' "${relay_entries[@]}" |
    sed -E 's/^[{]"([^"]+)",.*/\1/' |
    sort
)
if [[ "$(printf '%s\n' "${relay_names[@]}" | uniq | wc -l)" -ne "${#relay_names[@]}" ]]; then
  echo "collection relay audit: duplicate method contract" >&2
  exit 1
fi

for method in "${relay_names[@]}"; do
  if ! rg -q "\"${method}\"" "$vm_source"; then
    echo "collection relay audit: compiler method '${method}' has no VM dispatch contract" >&2
    exit 1
  fi
done

mapfile -t enum_relays < <(
  sed -n '/typedef enum CollectionRelay {/,/} CollectionRelay;/p' "$compiler_source" |
    grep -oE 'COLLECTION_RELAY_[A-Z_]+' |
    grep -v '^COLLECTION_RELAY_NONE$' |
    sort -u
)
mapfile -t table_relays < <(
  printf '%s\n' "${relay_entries[@]}" |
    grep -oE 'COLLECTION_RELAY_[A-Z_]+' |
    sort -u
)
if ! diff -u <(printf '%s\n' "${enum_relays[@]}") \
             <(printf '%s\n' "${table_relays[@]}") >/dev/null; then
  echo "collection relay audit: enum/table classification drift" >&2
  exit 1
fi

echo "${#relay_names[@]} collection relay contracts audited"
