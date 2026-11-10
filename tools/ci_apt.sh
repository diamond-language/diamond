#!/bin/sh
# Install CI dependencies, refreshing signed indexes after mirror/cache races.
set -eu
export DEBIAN_FRONTEND=noninteractive

apt() {
    apt-get -o Acquire::Retries=2 -o Acquire::http::Timeout=30 \
        -o Acquire::https::Timeout=30 -o Acquire::http::No-Cache=true \
        -o Acquire::https::No-Cache=true "$@"
}

attempt=1
while :; do
    # A partial update must not leave install using old indexes. In particular,
    # retrying install alone keeps requesting a superseded package after a 404.
    if apt -o APT::Update::Error-Mode=any update && apt install -y "$@"; then
        exit 0
    fi
    if [ "$attempt" -eq 3 ]; then
        echo "APT dependency bootstrap failed after $attempt attempts" >&2
        exit 1
    fi
    echo "APT dependency bootstrap failed; refreshing indexes before retry" >&2
    attempt=$((attempt + 1))
    sleep 5
done
