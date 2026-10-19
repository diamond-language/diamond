#!/usr/bin/env bash
# Upgrade an installed registry to a new bundle by swapping the `current`
# symlink. Take and verify a backup first. The previous release is kept only
# until the new one passes its health check; after that, git is the rollback.
# A release that fails to start or to become healthy is rolled back and set aside.
# Schema migrations run on startup and must be additive.
set -euo pipefail
[[ $EUID == 0 && $# == 2 && "$2" =~ ^[0-9a-f]{64}$ ]] || {
    echo 'usage (as root): upgrade.sh <bundle.tar.gz> <sha256>' >&2; exit 64;
}
bundle=$(realpath "$1")
printf '%s  %s\n' "$2" "$bundle" | sha256sum -c -
base=/opt/diamond-registry
[[ -L "$base/current" && -d /var/lib/diamond-registry ]] || {
    echo 'No installed registry; use install_initial.sh' >&2; exit 1;
}
previous=$(readlink "$base/current")
staging=$(mktemp -d /opt/.registry-upgrade.XXXXXXXX)
trap 'rm -rf -- "$staging"' EXIT
tar -xzf "$bundle" -C "$staging"
(cd "$staging" && sha256sum -c SHA256SUMS >/dev/null)
revision=$(<"$staging/REVISION")
[[ "$revision" =~ ^[0-9a-f]{40}$ ]] || exit 1
release="$base/releases/$revision"
[[ "$previous" != "$release" ]] || { echo "Revision $revision is already current" >&2; exit 1; }
[[ ! -e "$release" ]] || { echo 'Release directory already exists' >&2; exit 1; }
"$staging/bin/diamond" -e '1 + 1' >/dev/null
for archive in "$staging"/seed/*.tar; do "$staging/bin/facet" verify "$archive" >/dev/null; done
mv "$staging" "$release"
trap - EXIT
chown -R root:root "$release"
chmod 755 "$release" "$release/app" "$release/bin"
chmod -R a+rX "$release/app" "$release/bin"

healthy() {
    for _ in {1..30}; do
        if curl -fsS http://127.0.0.1:18120/health >/dev/null 2>&1; then return 0; fi
        sleep 1
    done
    return 1
}
ln -sfn "$release" "$base/.current-next"
mv -T "$base/.current-next" "$base/current"
# A restart that fails outright (a release that cannot exec) is rolled back the same way as one that never turns healthy.
if ! systemctl restart diamond-registry || ! healthy; then
    echo "Revision $revision failed to start or pass its health check; restoring $previous" >&2
    ln -sfn "$previous" "$base/.current-next"
    mv -T "$base/.current-next" "$base/current"
    systemctl restart diamond-registry || echo "restarting the previous release failed" >&2
    healthy || echo "the previous release is not healthy either" >&2
    # Move the failed release aside: left at $release it would make re-running the same bundle stop at "Release directory already exists".
    # The next successful upgrade removes it with the other old releases.
    mv -- "$release" "$release.failed-$(date +%s)"
    exit 1
fi
curl -fsS --resolve cuts.dilang.tech:443:127.0.0.1 https://cuts.dilang.tech/health
for old in "$base"/releases/*; do
    [[ "$old" == "$release" ]] || rm -rf -- "$old"
done
printf '\nUpgraded registry from %s to %s.\n' "$(basename "$previous")" "$revision"
