# cuts.dilang.tech launch preparation

The user selected `https://cuts.dilang.tech` on the existing droplet. Read-only
inspection on 2026-09-23 found a DNS typo: cuts.dilang.tech resolved to
142.39.192.149, while the droplet interface and modartist.app resolve to
**142.93.192.149**. The user is correcting the A record. Recheck DNS before TLS
activation. The same inspection found Caddy active
and enabled, existing routes for dilang.tech and Skindicate, and an active UFW
policy allowing SSH, HTTP, and HTTPS. No production configuration was changed
by this inspection. The public registry has not been deployed or seeded.

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
- Choose the external alert recipient/service and provision its credentials
  outside Git. Connect probes and expiry/storage/backup-age signals; deliver a
  test alert and confirm receipt.
- Build release binaries in QEMU matching the droplet OS. Install registry code
  and static catalog assets without modifying the running Skindicate checkout.
  The service working directory must contain `app.di`, `catalog.di`,
  `catalog.html`, `catalog.js`, and installed cuts.
- Validate and enable the registry service, then verify reboot activation and
  the real host firewall. Preserve other sites and check them after proxy reload.
- Issue a temporary scoped seed credential, publish the reviewed 24-cut inventory,
  verify public facet install/execute and locked reinstall, then revoke the seed
  credential. Inspect the public catalog against the inventory.
- Finalize 0.7 versus 0.8, release notes, tag, and announcement after verification.

The catalog reads live published rows and will show an empty state until releases
exist. The checked-in candidate inventory is not substituted for live availability.

## Current backup and alert choices

The user prefers **manual downloads to their laptop for now**. No laptop path,
automatic off-host schedule, or alert receiver has been selected. Do not represent
scheduled backups or automatic alert delivery as operational. Recovery can lose
all changes since the last manual snapshot; record when each verified copy was
made and repeat before upgrades, credential changes, and package publication.

With Python 3.12+ on the laptop, select a new private destination yourself and use:

```sh
python3 tools/fetch_registry_backup.py /your/chosen/new-backup-directory \
  --host root@modartist.app \
  --app /actual/registry/application/directory \
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
