#!/usr/bin/env bash
# Build a committed registry deployment bundle in the local Ubuntu QEMU guest.
set -euo pipefail
[[ $# == 1 ]] || { echo "usage: $0 <new-local-output-directory>" >&2; exit 64; }
repo=$(realpath "$(dirname "$0")/..")
port=${REGISTRY_VM_PORT:-2222}
known_hosts=${REGISTRY_VM_KNOWN_HOSTS:-$repo/../.vm-build/known_hosts}
[[ "$port" =~ ^[0-9]+$ ]] && ((port > 0 && port <= 65535)) || exit 64
[[ -f "$known_hosts" ]] || { echo 'Verified VM host keys are required' >&2; exit 1; }
git -C "$repo" diff --quiet && git -C "$repo" diff --cached --quiet || {
    echo 'Commit tracked changes before building a deployment bundle' >&2; exit 1;
}
revision=$(git -C "$repo" rev-parse HEAD)
mkdir -m 700 -- "$1"
output=$(realpath "$1")
ssh_options=(-F /dev/null -p "$port" -o BatchMode=yes -o ConnectTimeout=10
    -o StrictHostKeyChecking=yes -o "UserKnownHostsFile=$known_hosts")
remote=$(ssh "${ssh_options[@]}" builder@127.0.0.1 'mktemp -d /tmp/diamond-registry-release.XXXXXXXX')
[[ "$remote" =~ ^/tmp/diamond-registry-release\.[a-zA-Z0-9]+$ ]] || exit 1
cleanup() { ssh "${ssh_options[@]}" builder@127.0.0.1 "rm -rf -- '$remote'" || true; }
trap cleanup EXIT
git -C "$repo" archive "$revision" | ssh "${ssh_options[@]}" builder@127.0.0.1 "tar -xf - -C '$remote'"
ssh "${ssh_options[@]}" builder@127.0.0.1 bash -s -- "$remote" "$revision" <<'REMOTE'
set -euo pipefail
cd "$1"
. /etc/os-release
[[ "$ID" == ubuntu && "$VERSION_ID" == 26.04 && "$(uname -m)" == x86_64 ]] || {
    echo 'Registry deployment builds require Ubuntu 26.04 x86_64' >&2; exit 1;
}
make -j2 CFLAGS_RELEASE='-O2 -DNDEBUG -march=x86-64-v3' CFLAGS_DEBUG='-O2 -g -march=x86-64-v3' release
make CFLAGS_DEBUG='-O2 -g -march=x86-64-v3' facet
FACET_BIN="$PWD/build/facet" DIAMOND_BIN="$PWD/build/diamond" python3 packages/registry/http_test.py
mkdir -p payload/bin payload/app payload/deploy
cp build/diamond build/facet payload/bin/
cp applications/registry/{app.di,catalog.di,catalog.html,catalog.js,credentials.di,backup.py,monitor.py} payload/app/
FACET_BIN="$PWD/build/facet" tools/install_local_cuts.sh payload/app
python3 tools/prepare_registry_seed.py "$PWD/payload/seed" --expect docs/registry-launch-inventory.json
cp applications/registry/deploy/{registry.service,registry.env.example,nginx-loopback.conf.example,Caddyfile.registry} payload/deploy/
printf '%s\n' "$2" > payload/REVISION
(cd payload && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum) > payload/SHA256SUMS
tar -C payload -czf registry-deployment.tar.gz .
REMOTE
ssh "${ssh_options[@]}" builder@127.0.0.1 "cat '$remote/registry-deployment.tar.gz'" > "$output/registry-deployment.tar.gz.part"
mv "$output/registry-deployment.tar.gz.part" "$output/registry-deployment.tar.gz"
printf '%s\n' "$revision" > "$output/REVISION"
(cd "$output" && sha256sum registry-deployment.tar.gz > SHA256SUMS)
echo "Built revision $revision in $output"
