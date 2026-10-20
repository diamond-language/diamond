#!/usr/bin/env bash
# CI helper for `make test-database-dialects`: builds and runs it in an ubuntu:26.04 container (the source needs gcc 15's #embed, which the
# runner's own Ubuntu does not have). The container gets the host's docker socket, so the dialect scripts' `podman` (the docker client) starts
# the throwaway MariaDB, MySQL and PostgreSQL containers beside it, and the host network, so the ports they publish are on 127.0.0.1.
set -euo pipefail
cd "$(dirname "$0")/.."
exec docker run --rm --network host -v /var/run/docker.sock:/var/run/docker.sock -v "$PWD":/src ubuntu:26.04 bash -c '
set -euo pipefail
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gcc make git diffutils findutils ripgrep libssl-dev libsqlite3-dev libpq-dev zlib1g-dev libmariadb-dev libcrypt-dev bc curl docker.io >/dev/null
ln -s "$(command -v docker)" /usr/local/bin/podman
cp -r /src /work
cd /work
make test-database-dialects
'
