#!/usr/bin/env bash
# Manually download and verify a registry snapshot into a new local directory.
#
#   tools/fetch_registry_backup.sh DESTINATION --host root@your-host --app REMOTE_APP --data REMOTE_ROOT [--port 22]
#
# DESTINATION is a new directory on this machine (an existing one is never touched). The snapshot is
# made on the server by backup.di, streamed back as a tar, checked for anything but plain files
# below `snapshot/`, extracted, and finally restored into a scratch directory to prove it is usable.
# Set DIAMOND_BIN to use a diamond other than build/diamond, and SSH to substitute the ssh command.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
diamond="${DIAMOND_BIN:-$root/build/diamond}"
ssh_command="${SSH:-ssh}"

usage() { echo "usage: $0 DESTINATION --host HOST --app REMOTE_APP --data REMOTE_ROOT [--port PORT]" >&2; exit 2; }

destination=""; host=""; app=""; data=""; port=22
while [ $# -gt 0 ]; do
    case "$1" in
        --host) host="${2:?--host needs a value}"; shift 2 ;;
        --app) app="${2:?--app needs a value}"; shift 2 ;;
        --data) data="${2:?--data needs a value}"; shift 2 ;;
        --port) port="${2:?--port needs a value}"; shift 2 ;;
        -*) usage ;;
        *) [ -z "$destination" ] || usage; destination="$1"; shift ;;
    esac
done
[ -n "$destination" ] && [ -n "$host" ] && [ -n "$app" ] && [ -n "$data" ] || usage

fail() { echo "backup download failed: $*" >&2; exit 1; }

case "$port" in ''|*[!0-9]*) fail "SSH port must be between 1 and 65535" ;; esac
[ "$port" -ge 1 ] && [ "$port" -le 65535 ] || fail "SSH port must be between 1 and 65535"
case "$host" in -*|*[[:space:]]*) fail "invalid SSH host" ;; esac
case "$app" in /*) ;; *) fail "remote application and data paths must be absolute" ;; esac
case "$data" in /*) ;; *) fail "remote application and data paths must be absolute" ;; esac

umask 077
# mkdir without -p is exclusive: an existing destination is never changed or removed.
mkdir -m 700 "$destination" 2>/dev/null || fail "cannot create $destination (it may already exist)"
complete=false
cleanup() { $complete || rm -rf -- "$destination"; }
trap cleanup EXIT

app_dir="${app%/}"
quote() { printf "'%s'" "${1//\'/\'\\\'\'}"; }
# The server's own diamond runs backup.di (installed next to the app directory, in bin/).
remote_command="set -eu; work=\$(mktemp -d /tmp/diamond-registry-backup.XXXXXXXX); trap 'rm -rf -- \"\$work\"' EXIT; "
remote_command+="$(quote "$app_dir/../bin/diamond") $(quote "$app_dir/backup.di") backup $(quote "$data") \"\$work/snapshot\" >/dev/null; "
remote_command+="tar -C \"\$work\" -cf - snapshot"

archive="$destination/.transfer.tar"
"$ssh_command" -F /dev/null -p "$port" -o BatchMode=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=10 \
    "$host" "$remote_command" > "$archive" || fail "ssh failed"

# Only plain files and directories below snapshot/, with no absolute or parent-relative names.
names="$(tar -tf "$archive")" || fail "unreadable snapshot archive"
while IFS= read -r name; do
    case "$name" in
        snapshot|snapshot/*) ;;
        *) fail "unexpected snapshot archive contents" ;;
    esac
    trimmed="${name%/}"
    case "/$trimmed/" in
        */../*|*//*) fail "unexpected snapshot archive contents" ;;
    esac
done <<<"$names"
while IFS= read -r line; do
    case "${line:0:1}" in
        -|d) ;;
        *) fail "unexpected snapshot archive contents" ;;
    esac
done < <(tar -tvf "$archive")

tar -xf "$archive" -C "$destination" || fail "cannot extract the snapshot"
"$diamond" "$root/applications/registry/backup.di" restore "$destination/snapshot" "$destination/.restore-check" >/dev/null \
    || fail "the snapshot does not restore"
rm -rf -- "$destination/.restore-check" "$archive"
complete=true
echo "Verified snapshot: $destination/snapshot"
