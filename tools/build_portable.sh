#!/usr/bin/env bash
# Builds a portable d6: fully static (musl) executables of d6, facet, d6-lsp
# and d6-dap, per CPU architecture, that run on any Linux distribution with no
# dependency on the machine's libc, OpenSSL, SQLite, libpq, MariaDB client or zlib. It builds inside an Alpine
# container from the tracked files of this working tree (uncommitted edits
# included), so the host's build/ is left alone.
#
#   tools/build_portable.sh [OUTPUT_DIR]     default: build/portable
#
# Needs podman or docker. Writes OUTPUT_DIR/d6-linux-ARCH/ (ARCH from uname -m)
# holding d6, facet, d6-lsp and d6-dap, and the same as
# OUTPUT_DIR/d6-VERSION-linux-ARCH.tar.gz, the file a release attaches.
# Static glibc is not an option: its name-service lookups load shared
# libraries at run time (see docs/deployment.md).
set -euo pipefail
cd "$(dirname "$0")/.."

out="${1:-build/portable}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
arch="$(uname -m)"
image="${PORTABLE_IMAGE:-docker.io/library/alpine:3.23}"
engine="$(command -v podman || command -v docker || true)"
if [[ -z "$engine" ]]; then
    echo "build_portable: needs podman or docker" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
git ls-files -z | tar --null -T - -cf "$work/source.tar"
cp tools/build_portable_inside.sh "$work/inside.sh"

"$engine" run --rm -v "$work":/in:Z -v "$out":/out:Z -e ARCH="$arch" \
    -e JOBS="${JOBS:-10}" "$image" sh /in/inside.sh
echo "build_portable: wrote $out/d6-linux-$arch/ and its tarball"
