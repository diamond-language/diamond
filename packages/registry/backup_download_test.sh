#!/usr/bin/env bash
# Archive download verification with a controlled SSH transport: a verified restore, corruption,
# path traversal, and destination preservation.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
diamond="${DIAMOND_BIN:-$root/build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tool="$root/tools/fetch_registry_backup.sh"

# A snapshot as backup.di writes it: database, blobs and a manifest.
snapshot="$work/snapshot"
mkdir -p "$snapshot/blobs"
blob='verified backup blob'
digest="$(printf '%s' "$blob" | sha256sum | cut -d' ' -f1)"
printf '%s' "$blob" > "$snapshot/blobs/$digest"
"$diamond" -e "
db = SQLite3.open(\"$snapshot/registry.db\")
db.execute(\"CREATE TABLE releases (sha256 TEXT, size INTEGER)\")
db.execute(\"INSERT INTO releases VALUES (?, ?)\", [\"$digest\", ${#blob}])
db.close()" > /dev/null
database_digest="$(sha256sum "$snapshot/registry.db" | cut -d' ' -f1)"
printf '{"format":1,"blobs":{"%s":%d},"database_sha256":"%s"}\n' "$digest" "${#blob}" "$database_digest" > "$snapshot/manifest.json"

# Fake transports: ssh is replaced by a script that prints a tar to stdout.
good="$work/ssh-good"
cat > "$good" <<TRANSPORT
#!/usr/bin/env bash
# the arguments the tool passes: -F /dev/null -p PORT -o BatchMode=yes -o StrictHostKeyChecking=yes ...
[[ "\$1 \$2 \$3" == "-F /dev/null -p" ]] || exit 90
[[ " \$* " == *" StrictHostKeyChecking=yes "* ]] || exit 91
tar -C "$work" -cf - snapshot
TRANSPORT
chmod +x "$good"

run() { SSH="$1" DIAMOND_BIN="$diamond" bash "$tool" "${@:2}" --host operator@example.invalid --app /app --data /data; }

# A verified snapshot is kept.
run "$good" "$work/downloaded" > /dev/null
[[ "$(cat "$work/downloaded/snapshot/blobs/$digest")" == "$blob" ]]
[[ ! -e "$work/downloaded/.transfer.tar" && ! -e "$work/downloaded/.restore-check" ]]

# An existing destination is rejected and left alone.
if run "$good" "$work/downloaded" > /dev/null 2>&1; then echo "existing directory accepted" >&2; exit 1; fi
[[ "$(cat "$work/downloaded/snapshot/blobs/$digest")" == "$blob" ]]

# A corrupt snapshot is rejected and nothing is left behind.
printf 'corrupt' > "$snapshot/blobs/$digest"
if run "$good" "$work/bad" > /dev/null 2>&1; then echo "corrupt snapshot accepted" >&2; exit 1; fi
[[ ! -e "$work/bad" ]]
printf '%s' "$blob" > "$snapshot/blobs/$digest"

# An archive that tries to escape the destination is rejected.
mkdir "$work/evil"
printf x > "$work/evil/payload"
if tar --version 2>/dev/null | grep -q GNU; then
    (cd "$work/evil" && tar -cf "$work/malicious.tar" --transform 's,^payload,../escaped,' payload)
else
    (cd "$work/evil" && tar -cf "$work/malicious.tar" -s ',^payload,../escaped,' payload)
fi
bad="$work/ssh-bad"
printf '#!/usr/bin/env bash\ncat %q\n' "$work/malicious.tar" > "$bad"
chmod +x "$bad"
if run "$bad" "$work/escape" > /dev/null 2>&1; then echo "unexpected archive accepted" >&2; exit 1; fi
[[ ! -e "$work/escape" && ! -e "$work/escaped" ]]

# A symlink member is rejected too.
ln -s /etc/passwd "$work/evil/link"
(cd "$work/evil" && tar -cf "$work/linked.tar" link)
printf '#!/usr/bin/env bash\ncat %q\n' "$work/linked.tar" > "$bad"
if run "$bad" "$work/linked" > /dev/null 2>&1; then echo "symlink member accepted" >&2; exit 1; fi
[[ ! -e "$work/linked" ]]

echo "manual backup download: verified restore, corruption, traversal, and destination preservation passed"
