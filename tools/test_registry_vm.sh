#!/usr/bin/env bash
# Run the current source snapshot in an existing local QEMU guest over SSH.
set -euo pipefail
repo=$(realpath "$(dirname "$0")/..")
port=${REGISTRY_VM_PORT:-2222}
known_hosts=${REGISTRY_VM_KNOWN_HOSTS:-$repo/../.vm-build/known_hosts}
[[ "$port" =~ ^[0-9]+$ ]] && ((port > 0 && port <= 65535)) || {
    echo 'REGISTRY_VM_PORT must be between 1 and 65535' >&2; exit 1;
}
[[ -f "$known_hosts" ]] || { echo 'Set REGISTRY_VM_KNOWN_HOSTS to a verified host-key file' >&2; exit 1; }
ssh_options=(-F /dev/null -p "$port" -o BatchMode=yes -o ConnectTimeout=10
    -o StrictHostKeyChecking=yes -o "UserKnownHostsFile=$known_hosts")
archive=$(mktemp /tmp/diamond-registry-source.XXXXXXXX.tar)
trap 'rm -f "$archive"' EXIT
# Include pending source changes for verification; ignored build outputs and .git
# are excluded. Review nonignored untracked files before transferring.
# Git config and the sibling application are not included.
(cd "$repo" && git ls-files --cached --others --exclude-standard -z |
    tar --null -T - -cf "$archive")
remote=$(ssh "${ssh_options[@]}" builder@127.0.0.1 'mktemp -d /tmp/diamond-registry-staging.XXXXXXXX')
[[ "$remote" =~ ^/tmp/diamond-registry-staging\.[a-zA-Z0-9]+$ ]] || exit 1
cleanup() {
    rm -f "$archive"
    ssh "${ssh_options[@]}" builder@127.0.0.1 "rm -rf -- '$remote'" || true
}
trap cleanup EXIT
ssh "${ssh_options[@]}" builder@127.0.0.1 "tar -xf - -C '$remote'" < "$archive"
ssh "${ssh_options[@]}" builder@127.0.0.1 bash -s -- "$remote" <<'REMOTE'
set -euo pipefail
cd "$1"
command -v nginx >/dev/null || [[ -x /usr/sbin/nginx ]] || {
    echo 'Install nginx in the staging guest first' >&2; exit 1;
}
make -j2
make facet
make test-registry-nginx
make test-registry-http
REMOTE
