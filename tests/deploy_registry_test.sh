#!/usr/bin/env bash
# tools/deploy_registry.sh decision logic, against a throwaway repository, a stub `build/diamond`
# that exits with a chosen upgrade-check verdict, and a no-op `make`: no host, VM or network.
set -euo pipefail

script="$(cd "$(dirname "$0")/.." && pwd)/tools/deploy_registry.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

git init -q --bare "$work/origin.git"
repo="$work/repo"
mkdir -p "$repo/tools" "$repo/build" "$work/bin" "$work/backups"
cp "$script" "$repo/tools/deploy_registry.sh"
printf '#!/bin/sh\necho "stub upgrade check (verdict ${FAKE_VERDICT})" >&2\nexit "${FAKE_VERDICT}"\n' > "$repo/build/diamond"
chmod +x "$repo/build/diamond"
cp "$repo/build/diamond" "$repo/build/facet"
printf '#!/bin/sh\nexit 0\n' > "$work/bin/make"
chmod +x "$work/bin/make"
(
    cd "$repo"
    git init -q -b main
    git config user.email test@example.invalid
    git config user.name test
    printf 'build/\n' > .gitignore
    git add -A
    git commit -q -m initial
    git remote add origin "$work/origin.git"
    git push -q origin main
)

# Prints "<exit status>|<output>" for one invocation.
deploy() {
    local verdict="$1"; shift
    local output status=0
    output="$(cd "$repo" && PATH="$work/bin:$PATH" FAKE_VERDICT="$verdict" REGISTRY_HOST=root@127.0.0.1 \
        REGISTRY_BACKUP_DIR="$work/backups" bash tools/deploy_registry.sh "$@" 2>&1)" || status=$?
    printf '%s|%s' "$status" "$output"
}

expect() {
    local label="$1" result="$2" status="$3" fragment="$4"
    if [[ "${result%%|*}" != "$status" || "${result#*|}" != *"$fragment"* ]]; then
        echo "FAIL: $label (wanted exit $status containing '$fragment')" >&2
        echo "$result" >&2
        exit 1
    fi
}

# The check itself failing (exit 1: host unreachable, unknown revision...) must not read as "nothing to do",
# with or without --force.
expect "failed check" "$(deploy 1 --dry-run)" 1 "The upgrade check failed (exit 1)"
expect "failed check, forced" "$(deploy 1 --dry-run --force)" 1 "The upgrade check failed (exit 1)"
expect "unexpected verdict" "$(deploy 70 --dry-run)" 1 "The upgrade check failed (exit 70)"
# NONE and OPTIONAL stop quietly unless forced.
expect "none" "$(deploy 0 --dry-run)" 0 "Not upgrading (verdict 0)"
expect "optional" "$(deploy 10 --dry-run)" 0 "Not upgrading (verdict 10)"
expect "optional, forced" "$(deploy 10 --dry-run --force)" 0 "Dry run: nothing was changed."
# REQUIRED goes ahead; every mutating step is only printed, and the backup name carries the time of day.
required="$(deploy 20 --dry-run)"
expect "required" "$required" 0 "Dry run: nothing was changed."
expect "required, upload printed" "$required" 0 "+ scp -q applications/registry/deploy/upgrade.sh"
if ! grep -Eq 'cutbackup-[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{6}-pre-' <<<"$required"; then
    echo "FAIL: backup directory name has no time of day" >&2
    echo "$required" >&2
    exit 1
fi

echo "deploy_registry tests passed"
