#!/usr/bin/env bash
# Initial registry installation on the dilang.tech nginx host. Never an upgrade.
# nginx owns ports 80/443 (nginx-host.conf.example installed as
# /etc/nginx/conf.d/00-common.conf) and certbot has issued the cuts.dilang.tech
# certificate through the /var/www/letsencrypt webroot.
set -euo pipefail
[[ $EUID == 0 && $# == 2 && "$2" =~ ^[0-9a-f]{64}$ ]] || {
    echo 'usage (as root): install_initial.sh <bundle.tar.gz> <sha256>' >&2; exit 64;
}
bundle=$(realpath "$1")
printf '%s  %s\n' "$2" "$bundle" | sha256sum -c -
base=/opt/diamond-registry
unit=/etc/systemd/system/diamond-registry.service
[[ ! -e "$base/current" && ! -e "$unit" && ! -e /var/lib/diamond-registry ]] || {
    echo 'Existing registry state detected; use a reviewed upgrade procedure' >&2; exit 1;
}
systemctl is-active --quiet nginx
[[ -f /etc/nginx/conf.d/00-common.conf ]] || {
    echo 'Install nginx-host.conf.example as /etc/nginx/conf.d/00-common.conf first' >&2; exit 1;
}
[[ -f /etc/letsencrypt/live/cuts.dilang.tech/fullchain.pem ]] || {
    echo 'Issue the certificate first: certbot certonly --webroot -w /var/www/letsencrypt -d cuts.dilang.tech' >&2; exit 1;
}
staging=$(mktemp -d /opt/.registry-install.XXXXXXXX)
trap 'rm -rf -- "$staging"' EXIT
tar -xzf "$bundle" -C "$staging"
(cd "$staging" && sha256sum -c SHA256SUMS >/dev/null)
revision=$(<"$staging/REVISION")
[[ "$revision" =~ ^[0-9a-f]{40}$ ]] || exit 1
"$staging/bin/diamond" -e '1 + 1' >/dev/null
"$staging/bin/facet" verify "$staging/seed/logger.tar" >/dev/null
mkdir -p "$base/releases"
[[ ! -e "$base/releases/$revision" ]] || { echo 'Release directory already exists' >&2; exit 1; }
mv "$staging" "$base/releases/$revision"
trap - EXIT
release="$base/releases/$revision"
chown -R root:root "$release"
chmod 755 "$release" "$release/app" "$release/bin"
chmod -R a+rX "$release/app" "$release/bin"
ln -s "$release" "$base/current"
if ! id diamond-registry >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin diamond-registry
fi
install -d -m 700 -o diamond-registry -g diamond-registry /var/lib/diamond-registry \
    /var/lib/diamond-registry/blobs /var/lib/diamond-registry/staging
sed -e 's|/opt/diamond/applications/registry|/opt/diamond-registry/current/app|' \
    -e 's|/opt/diamond/build/diamond|/opt/diamond-registry/current/bin/diamond|' \
    "$release/deploy/registry.service" > "$unit"
sed 's|/opt/diamond/build/facet|/opt/diamond-registry/current/bin/facet|' \
    "$release/deploy/registry.env.example" > /etc/diamond-registry.env
chmod 600 /etc/diamond-registry.env
systemd-analyze verify "$unit"
systemctl daemon-reload
systemctl enable --now diamond-registry
for attempt in {1..30}; do
    if curl -fsS http://127.0.0.1:18120/health >/dev/null 2>&1; then break; fi
    sleep 1
done
curl -fsS http://127.0.0.1:18120/health
[[ ! -e /etc/nginx/conf.d/diamond-registry.conf ]] || {
    echo 'Registry nginx configuration already exists' >&2; exit 1;
}
install -m 644 "$release/deploy/nginx.conf.example" /etc/nginx/conf.d/diamond-registry.conf
if ! nginx -t || ! systemctl reload nginx; then
    rm /etc/nginx/conf.d/diamond-registry.conf
    systemctl reload nginx
    exit 1
fi
curl -fsS --resolve cuts.dilang.tech:443:127.0.0.1 https://cuts.dilang.tech/health
printf '\nInstalled registry revision %s.\n' "$revision"
printf 'Verify external HTTPS and existing sites before seeding. No packages have been published.\n'
