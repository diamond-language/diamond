#!/usr/bin/env bash
# Exercise unit migration and rollback in a disposable filesystem, never systemd.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir "$work/bin"
for command in chown systemd-analyze sleep; do
    printf '#!/bin/sh\nexit 0\n' > "$work/bin/$command"
done
cat > "$work/bin/systemctl" <<'MOCK'
#!/usr/bin/env bash
if [[ "$1" == restart && "$(readlink "$FIXTURE/base/current")" == "$FIXTURE/base/releases/$NEW" && "${FAIL_START:-}" == 1 ]]; then exit 1; fi
MOCK
cat > "$work/bin/curl" <<'MOCK'
#!/usr/bin/env bash
if [[ "$(readlink "$FIXTURE/base/current")" == "$FIXTURE/base/releases/$NEW" && "${FAIL_HEALTH:-}" == 1 ]]; then exit 1; fi
echo healthy
MOCK
chmod +x "$work/bin/"*
export PATH="$work/bin:$PATH"
export NEW=2222222222222222222222222222222222222222
for scenario in success restart-failure health-failure; do
    export FIXTURE="$work/$scenario"
    mkdir -p "$FIXTURE/base/releases/old/bin" "$FIXTURE/base/releases/old/app" "$FIXTURE/data" "$FIXTURE/payload/"{app,bin,seed,deploy}
    ln -s "$FIXTURE/base/releases/old" "$FIXTURE/base/current"
    printf 'ExecStart=%s/base/current/bin/diamond app.di\n' "$FIXTURE" > "$FIXTURE/unit.service"
    cp "$FIXTURE/unit.service" "$FIXTURE/expected-old.service"
    cp "$root/applications/registry/deploy/registry.service" "$FIXTURE/payload/deploy/"
    for binary in diamond facet; do
        printf '#!/bin/sh\nexit 0\n' > "$FIXTURE/payload/bin/$binary"
        chmod +x "$FIXTURE/payload/bin/$binary"
    done
    touch "$FIXTURE/payload/seed/logger.tar"
    printf '%s\n' "$NEW" > "$FIXTURE/payload/REVISION"
    (cd "$FIXTURE/payload" && find . -type f ! -name SHA256SUMS -exec sha256sum {} + > SHA256SUMS)
    tar -C "$FIXTURE/payload" -czf "$FIXTURE/bundle.tar.gz" .
    sha=$(sha256sum "$FIXTURE/bundle.tar.gz" | cut -d' ' -f1)
    # Replace only root-only host locations and the UID guard in this test copy.
    sed -e 's/\$EUID == 0/1 == 1/' \
        -e "s|/opt/diamond-registry|$FIXTURE/base|g" \
        -e "s|/var/lib/diamond-registry|$FIXTURE/data|g" \
        -e "s|/opt/.registry-upgrade.XXXXXXXX|$FIXTURE/staging.XXXXXXXX|g" \
        -e "s|/etc/systemd/system/diamond-registry.service|$FIXTURE/unit.service|g" \
        "$root/applications/registry/deploy/upgrade.sh" > "$FIXTURE/upgrade.sh"
    export FAIL_START=0 FAIL_HEALTH=0
    [[ "$scenario" != restart-failure ]] || FAIL_START=1
    [[ "$scenario" != health-failure ]] || FAIL_HEALTH=1
    status=0
    bash "$FIXTURE/upgrade.sh" "$FIXTURE/bundle.tar.gz" "$sha" > "$FIXTURE/log" 2>&1 || status=$?
    if [[ "$scenario" == success ]]; then
        [[ "$status" == 0 && "$(readlink "$FIXTURE/base/current")" == "$FIXTURE/base/releases/$NEW" ]]
        grep -q 'app.d6' "$FIXTURE/unit.service"
        [[ ! -d "$FIXTURE/base/releases/old" ]]
    else
        [[ "$status" != 0 && "$(readlink "$FIXTURE/base/current")" == "$FIXTURE/base/releases/old" ]]
        cmp "$FIXTURE/unit.service" "$FIXTURE/expected-old.service"
        [[ ! -d "$FIXTURE/base/releases/$NEW" ]]
    fi
done
echo 'registry upgrade: d6 unit migration and restart/health rollback passed'

# Execute the generated remote backup command against an old/new operator layout.
# Restore validation itself is covered by backup_download_test.sh; here it is a stub.
mkdir -p "$work/backup/app" "$work/backup/bin"
cat > "$work/backup/bin/diamond" <<'MOCK'
#!/usr/bin/env bash
printf '%s\n' "$1" > "$CHOICE"
mkdir -p "$4"
echo snapshot > "$4/marker"
MOCK
cat > "$work/bin/ssh" <<'MOCK'
#!/usr/bin/env bash
exec bash -c "${@: -1}"
MOCK
printf '#!/bin/sh\nexit 0\n' > "$work/bin/restore"
chmod +x "$work/backup/bin/diamond" "$work/bin/ssh" "$work/bin/restore"
export CHOICE="$work/choice"
touch "$work/backup/app/backup.di"
for extension in di d6; do
    touch "$work/backup/app/backup.$extension"
    SSH="$work/bin/ssh" DIAMOND_BIN="$work/bin/restore" bash "$root/tools/fetch_registry_backup.sh" \
        "$work/download-$extension" --host fixture --app "$work/backup/app" --data "$work/data" >/dev/null
    [[ "$(<"$CHOICE")" == "$work/backup/app/backup.$extension" ]]
done
echo 'registry backup: legacy di fallback and preferred d6 entry point passed'
