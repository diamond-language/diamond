#!/usr/bin/env bash
set -euo pipefail

# Hash the hashes rather than concatenating file contents so file boundaries
# are unambiguous. The input list is supplied in a stable order by Makefile.
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$@" | sha256sum | awk '{print $1}'
elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$@" | shasum -a 256 | awk '{print $1}'
else
    echo "diamond: SHA-256 utility required for AOT cache fingerprinting" >&2
    exit 1
fi
