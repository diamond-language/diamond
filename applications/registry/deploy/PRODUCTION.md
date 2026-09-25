# cuts.dilang.tech production record

The public registry is live at **https://cuts.dilang.tech** as of 2026-09-23.
The corrected A record is **142.93.192.149**. External health and catalog
requests succeeded. Since 2026-09-25 nginx terminates TLS with certbot
certificates (see "Proxy and certificates" below); Caddy was retired.

Runtime revision: `26758cbfdd516860c21944d4bbfbcb2321947f18` (upgraded
2026-09-25 with `deploy/upgrade.sh` for cut show pages, after a verified
laptop snapshot; previously `822114807fbc9cbdd579f29fec4e0958da8b78df` from
2026-09-24, launch revision `f90c10bd`), built in the local Ubuntu 26.04 QEMU guest with x86-64-v3 release
flags. Only the current release is kept on the host; git is the rollback. Schema
migration `2026092401` (nullable `releases.maintainers`) is additive. A verified
snapshot was taken immediately before the upgrade. Registry files live under
`/opt/diamond-registry/releases/<revision>` with a `current` symlink; application
state is `/var/lib/diamond-registry`. This separate root avoids the existing
Skindicate application's restricted `/opt/diamond` directory.

The registry and nginx services are active and enabled. The registry runs
as `diamond-registry` with `ProtectSystem=strict` and `NoNewPrivileges=yes`.
The droplet's other sites (the Skindicate app on skindicate.art and
modartist.app) were checked after activation.

**Always address the host as `root@dilang.tech`** in Diamond tooling, scripts,
and docs. The droplet currently also hosts Skindicate, which is expected to move
to its own server; Diamond deployments must keep working when it does and must
never target a Skindicate hostname.

**Reboot drill, 2026-09-24:** after a verified snapshot, the host was rebooted at
05:40:15Z and booted at 05:40:24Z. Caddy (since retired), nginx, `diamond-registry`, and
`skindicate` started unattended with no failed units; the registry passed its
loopback health check; all four sites returned their usual statuses externally;
and `verify_registry_launch.py` reinstalled all 18 cuts. Outage was about 30 s.

**Certificate renewal:** certbot's `certbot.timer` renews each certificate about
30 days before expiry through the `/var/www/letsencrypt` webroot, and
`/etc/letsencrypt/renewal-hooks/deploy/reload-nginx` reloads nginx afterwards.
The certificates issued 2026-09-25 expire 2026-12-24, so the first real renewals
are due around 2026-11-24. `certbot renew --dry-run --no-random-sleep-on-renew`
succeeded for all four names on 2026-09-25; confirm the new expiry
(`certbot certificates`) after the first real renewal.

The initial seed contained 24 cuts. At the operator's request, `dials`,
`active_auth`, `active_discussion`, `active_karma`, `active_social`, and
`active_tagging` were subsequently unpublished through audited takedowns.
The public launch inventory now contains **18 cuts**. On 2026-09-24 each gained a
patch release declaring `maintainers` (the current seed inventory), published with
a one-hour name-scoped credential that was then revoked. The 18 launch versions,
which predate `maintainers`, were then taken down ("Superseded by maintained
release") with a ten-minute admin credential, also revoked; each cut now serves
only its maintained x.y.1 release, and the catalog shows one row per cut.
facet 0.7.0 rejects archives that declare `maintainers`, so it cannot install
the current releases; use a newer facet. `verify_registry_launch.py` passed. No retained cut depends
on the removed cuts. Takedowns hide metadata and archive downloads while retaining
internal audit records and immutable version tombstones; those exact versions
cannot be republished. The source packages remain in the repository.
The publication and takedown credentials were temporary and have been revoked.
Initial published ownership belongs to `diamond-language`.

External HTTPS verification confirmed exactly 18 catalog releases, 404 responses
for each removed release's metadata and archive, and successful facet resolution,
digest checks, loading, and locked reinstall of every retained cut. The revised
seed also passed local reproducibility and dependency-closure checks.

## Host disk and logs

The droplet has 25 GB. As of 2026-09-24 the journal is capped at 100M (10M files,
14 days) via `/etc/systemd/journald.conf.d/size.conf`; rsyslog and nginx logs
rotate daily or at 10M, keeping 2 and 3 copies, with logrotate running hourly
(`logrotate.timer.d/hourly.conf`). Do not keep old application binaries or
releases on the host. Skindicate's uploads (~8.3G) are the largest real data.

## Proxy and certificates

nginx owns ports 80 and 443 for every site on the host: skindicate.art,
modartist.app (a redirect), dilang.tech (static files), and cuts.dilang.tech.
Files in `/etc/nginx/conf.d/`:

- `00-common.conf` (from `nginx-host.conf.example`): TLS settings, the port-80
  server that answers ACME HTTP-01 challenges from `/var/www/letsencrypt` and
  redirects everything else to HTTPS, and a TLS default that refuses unknown names.
- `dilang.conf` (also in `nginx-host.conf.example`): the static dilang.tech site.
- `diamond-registry.conf` (from `nginx.conf.example`): cuts.dilang.tech with the
  upload/read/write rate limits and request buffering.
- `skindicate.conf`: Skindicate's own sites, kept in that application's repository.

Each hostname has its own certbot certificate (`certbot certificates`). To add a
hostname, point DNS at the host, run `certbot certonly --webroot -w
/var/www/letsencrypt -d <name> --cert-name <name>`, then add its server block.
Validate with `nginx -t` before `systemctl reload nginx`. Client identity for
rate limits is the TCP peer; no forwarded headers are trusted. The registry stays
on port 18120 and Skindicate on 18110; the host firewall allows only SSH, 80, and
443. Caddy is installed but stopped and disabled; its last configuration and the
pre-migration nginx/Caddy configs are in `/root/proxy-migration-20260925T050335Z`.

## Remaining inputs and checks

- Launch publisher policy is confirmed: operator-approved maintainers with
  scoped credentials; no self-service registration. Issue only needed name scopes
  with an expiry and retain the credential lifecycle audit trail.
- The current backup choice is manual downloads to the laptop; use the procedure
  below and verify a downloaded snapshot. Retention and recovery targets remain
  operator decisions; automatic off-host backups are not configured.
- Automatic alerts are deferred by the operator as of 2026-09-23. Continue
  manual probes; selecting a receiver and verifying delivery is follow-up work,
  not a blocker for this release.
- Reboot drill passed 2026-09-24 (above). certbot renewal is scheduled and
  dry-run tested but not yet observed: check the expiries after 2026-11-24.
- Finalize 0.7 versus 0.8, release notes, tag, and announcement after verification.

The catalog reads live published rows; the checked-in inventory records the
reviewed public selection.

## Current backup and alert choices

The user prefers **manual downloads to their laptop for now**. The first snapshot
was downloaded to the operator-selected `~/Projects/diamond-lang/cutbackup/snapshot`
and verified by a temporary local restore on 2026-09-23T18:42:37+00:00.
Database integrity, foreign keys, archive sizes and digests passed. The snapshot
contains 18 public releases and six retained takedowns (24 archive blobs total);
its parent directory is private (0700). Future downloads require a new directory.
Automatic alerts are explicitly deferred; no receiver or automatic off-host
backup schedule is configured. Do not represent
scheduled backups or automatic alert delivery as operational. Recovery can lose
all changes since the last manual snapshot; record when each verified copy was
made and repeat before upgrades, credential changes, and package publication.

With Python 3.12+ on the laptop, select a new private destination yourself and use:

```sh
python3 tools/fetch_registry_backup.py /your/chosen/new-backup-directory \
  --host root@dilang.tech \
  --app /opt/diamond-registry/current/app \
  --data /var/lib/diamond-registry
```

Replace the application directory with the installed path containing `backup.py`.
The helper uses existing SSH keys and strict host-key verification. It snapshots
the running registry into a temporary directory on the server, streams the copy
over SSH, extracts it with Python's data-only tar filter, and performs a local
restore verification. It reports success only after database/blob checks pass.
The server temporary copy and local verification copy are removed; the retained
snapshot includes credential digests and must stay private. The destination must
not exist, and a failed transfer removes only the newly created destination.
No transfer is made until an operator supplies the destination and remote paths.

To rehearse recovery without touching a running service:

```sh
python3 applications/registry/backup.py restore \
  /your/chosen/backup-directory/snapshot /your/chosen/new-recovery-directory
```

Then run the recovered registry in QEMU, inspect release and credential state,
and perform a facet installation. Follow the service restore procedure when
switching real traffic. Manually run `monitor.py https://cuts.dilang.tech` until
external alert delivery is selected; a successful manual probe does not establish
continuous monitoring.

## Deployment tools

`tools/build_registry_vm.sh <new-output-directory>` builds the committed revision
in QEMU, runs the release-binary HTTPS integration suite, verifies the selected
seed, and produces `registry-deployment.tar.gz`, `SHA256SUMS`, and `REVISION`.
The QEMU guest must match Ubuntu 26.04 x86_64; the target CPU must support x86-64-v3.

`applications/registry/deploy/install_initial.sh <bundle.tar.gz> <sha256>` is a
root-only initial-install helper for the nginx host. It verifies bundle
checksums, installs the dedicated service and the registry's nginx server block,
and removes that block again if `nginx -t` or the reload fails. It refuses existing registry
state. Upgrade an installed registry with
`applications/registry/deploy/upgrade.sh <bundle.tar.gz> <sha256>` after a verified
backup: it stages the release, swaps `current`, restarts, restores the previous
release if the health check fails, and otherwise deletes older releases.
It does not publish packages or configure backup destinations and alert delivery.

For an explicitly approved new seed, use `tools/publish_registry_seed.py` with the
HTTPS registry URL, `--seed <verified-seed-directory>`, and
`--credentials <private-credential-json>`. Issue short-lived name-scoped credentials
with `credentials.di`; revoke them after the operation and remove the private file.
The publisher verifies archive identities and respects the live write rate limit.
Do not reuse the original deployed 24-cut seed: use the current 18-cut selection.

Run `python3 tools/verify_registry_launch.py https://cuts.dilang.tech` to resolve
all selected cuts into a temporary clean consumer, compare the lock against the
reviewed inventory, load every cut, and reinstall from the unchanged lockfile.
