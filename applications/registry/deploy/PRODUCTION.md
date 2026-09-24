# cuts.dilang.tech production record

The public registry is live at **https://cuts.dilang.tech** as of 2026-09-23.
The corrected A record is **142.93.192.149**. Caddy issued a publicly trusted
HTTPS certificate, and external health and catalog requests succeeded.

Runtime revision: `8f7481d638bb3d6504380c2d943902a50a5de54b` (upgraded
2026-09-24 with `deploy/upgrade.sh`, via `c93b7f22`, `518251b1`, and `c8c55b20`,
from the launch revision `f90c10bd`), built in the local Ubuntu 26.04 QEMU guest with x86-64-v3 release
flags. Only the current release is kept on the host; git is the rollback. Schema
migration `2026092401` (nullable `releases.maintainers`) is additive. A verified
snapshot was taken immediately before the upgrade. Registry files live under
`/opt/diamond-registry/releases/<revision>` with a `current` symlink; application
state is `/var/lib/diamond-registry`. This separate root avoids the existing
Skindicate application's restricted `/opt/diamond` directory.

The registry, nginx, and Caddy services are active and enabled. The registry runs
as `diamond-registry` with `ProtectSystem=strict` and `NoNewPrivileges=yes`.
Existing dilang.tech, skindicate.art, and modartist.app routes were checked after
activation.

**Reboot drill, 2026-09-24:** after a verified snapshot, the host was rebooted at
05:40:15Z and booted at 05:40:24Z. Caddy, nginx, `diamond-registry`, and
`skindicate` started unattended with no failed units; the registry passed its
loopback health check; all four sites returned their usual statuses externally;
and `verify_registry_launch.py` reinstalled all 18 cuts. Outage was about 30 s.

**Certificate renewal:** Caddy's ACME renewal-info checks run cleanly for all four
names, with no TLS errors logged. No certificate has yet been renewed on this
host; the first scheduled renewal is modartist.app around 2026-10-30, then
dilang.tech (~11-17), skindicate.art (~11-18), and cuts.dilang.tech (~11-22),
each expiring about a month later. Confirm the new expiry after the first one.

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

## Fit the existing proxy

## Host disk and logs

The droplet has 25 GB. As of 2026-09-24 the journal is capped at 100M (10M files,
14 days) via `/etc/systemd/journald.conf.d/size.conf`; rsyslog and nginx logs
rotate daily or at 10M, keeping 2 and 3 copies, with logrotate running hourly
(`logrotate.timer.d/hourly.conf`). Do not keep old application binaries or
releases on the host. Skindicate's uploads (~8.3G) are the largest real data.

## Fit the existing proxy

Keep Caddy on ports 80 and 443. `Caddyfile.registry` is an additive site block;
it must be merged with, not replace, the existing Caddyfile. Caddy handles
[automatic HTTPS and certificate renewal](https://caddyserver.com/docs/automatic-https).
Verify the issued certificate and external probe after activation and monitor
expiry and renewal errors; merely enabling Caddy is not a renewal drill.

Use `nginx-loopback.conf.example` inside nginx's `http` context. It listens only
on 127.0.0.1:18121 and applies the same upload/read/write policy as the directly
terminated HTTPS template. Disable nginx's default public listener before starting
it: Caddy already owns 80/443. The registry stays on port 18120. Do not open either
backend port in the host firewall.

The loopback nginx layer trusts forwarded client identity only from 127.0.0.1.
Caddy's default reverse proxy behavior
[replaces untrusted incoming forwarded headers](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy).
Do not add broad trusted-proxy networks or expose the intermediate nginx listener.
Validate the complete configuration with `nginx -t` and `caddy validate` before
reload. The complete Caddy → nginx → registry chain passed in QEMU on 2026-09-23,
including HTTPS publishing/installing and rate limits under spoofed forwarded-IP
headers. Validate the installed configuration and public traffic again on the host.

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
- Reboot drill passed 2026-09-24 (above). Certificate renewal is scheduled but
  not yet observed: check the modartist.app expiry after 2026-10-30.
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
  --host root@modartist.app \
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
root-only initial-install helper for the existing Caddy host. It verifies bundle
checksums, installs the dedicated service and loopback nginx layer, and backs up
and validates Caddy configuration before reload. It refuses existing registry
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
