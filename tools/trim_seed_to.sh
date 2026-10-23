#!/usr/bin/env bash
# Copy the named cuts from a prepared registry seed into a new seed directory with a matching, trimmed inventory.
#
#   tools/trim_seed_to.sh SOURCE_SEED NEW_SEED CUT...
#
# tools/publish_registry_seed.d6 publishes every cut in a seed's inventory, and a published version can never be replaced, so a release
# of a few cuts publishes from a seed trimmed to exactly those. Every archive's size and SHA-256 is checked against its inventory
# entry while copying, and the cuts keep the source inventory's order (dependencies first). NEW_SEED must not exist.
set -euo pipefail

[[ $# -ge 3 ]] || { echo "usage: $0 SOURCE_SEED NEW_SEED CUT..." >&2; exit 64; }
source_seed="$1"; new_seed="$2"; shift 2
[[ -f "$source_seed/inventory.json" ]] || { echo "no inventory.json in $source_seed" >&2; exit 1; }
[[ ! -e "$new_seed" ]] || { echo "$new_seed already exists" >&2; exit 1; }

python3 - "$source_seed" "$new_seed" "$@" <<'PYEOF'
import hashlib, json, os, shutil, sys

source, target, *wanted = sys.argv[1:]
inventory = json.load(open(os.path.join(source, "inventory.json")))
by_name = {cut["name"]: cut for cut in inventory["cuts"]}
missing = [name for name in wanted if name not in by_name]
if missing:
    sys.exit("not in the source inventory: " + ", ".join(missing))
chosen = [cut for cut in inventory["cuts"] if cut["name"] in wanted]
os.makedirs(target, mode=0o700)
for cut in chosen:
    data = open(os.path.join(source, cut["archive"]), "rb").read()
    if len(data) != cut["size"] or hashlib.sha256(data).hexdigest() != cut["sha256"]:
        sys.exit("archive differs from its inventory entry: " + cut["name"])
    shutil.copy(os.path.join(source, cut["archive"]), os.path.join(target, cut["archive"]))
trimmed = {key: value for key, value in inventory.items() if key != "cuts"}
trimmed["cuts"] = chosen
with open(os.path.join(target, "inventory.json"), "w") as out:
    json.dump(trimmed, out, indent=2, sort_keys=True)
    out.write("\n")
for cut in chosen:
    print("%-16s %-8s %s" % (cut["name"], cut["version"], cut["sha256"][:16]))
PYEOF
