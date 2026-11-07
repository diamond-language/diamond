#!/usr/bin/env bash
# Runs packaging/portable_tools_smoke.sh with the portable tools
# (tools/build_portable.sh) inside clean containers of several Linux
# distributions, to check the same static binaries work on each. Each
# container gets only what d6 expects of any host: CA certificates, plus curl
# and bash for facet and the new project's test.sh.
#
#   tools/test_portable_distros.sh [TOOLS_DIR]   default: build/portable/d6-linux-$(uname -m)
#
# PORTABLE_DISTROS overrides the image list (space separated).
# PORTABLE_SMOKE_OFFLINE=1 skips the checks that need the network.
set -euo pipefail
cd "$(dirname "$0")/.."

tools="$(cd "${1:-build/portable/d6-linux-$(uname -m)}" && pwd)"
engine="$(command -v podman || command -v docker || true)"
if [[ -z "$engine" ]]; then
    echo "test_portable_distros: needs podman or docker" >&2
    exit 1
fi
images="${PORTABLE_DISTROS:-docker.io/library/ubuntu:26.04 docker.io/library/ubuntu:22.04 \
docker.io/library/debian:12 docker.io/library/fedora:44 docker.io/library/archlinux:latest \
docker.io/library/alpine:3.23 docker.io/opensuse/leap:15.6}"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT

failed=0
for image in $images; do
    # Arch Linux publishes x86-64 images only.
    if [[ "$image" == *archlinux* && "$(uname -m)" != x86_64 ]]; then
        echo "skip $image (no $(uname -m) image)"
        continue
    fi
    # Debian-family and Alpine images ship without CA certificates or curl,
    # and Alpine without bash; the others already carry all three.
    case "$image" in
        *ubuntu*|*debian*) setup="apt-get update -qq && apt-get install -y -qq ca-certificates curl >/dev/null" ;;
        *alpine*) setup="apk add --no-cache -q ca-certificates curl bash" ;;
        *) setup="true" ;;
    esac
    if "$engine" run --rm -v "$tools":/opt/d6:ro,Z \
        -v "$PWD/packaging/portable_smoke.d6":/smoke.d6:ro,Z \
        -v "$PWD/packaging/portable_tools_smoke.sh":/smoke.sh:ro,Z \
        -e PORTABLE_SMOKE_OFFLINE="${PORTABLE_SMOKE_OFFLINE:-}" \
        "$image" sh -c "$setup && PATH=/opt/d6:\$PATH sh /smoke.sh" > "$log" 2>&1; then
        echo "pass $image"
    else
        echo "FAIL $image"
        sed 's/^/    /' "$log" | tail -8
        failed=1
    fi
done
exit "$failed"
