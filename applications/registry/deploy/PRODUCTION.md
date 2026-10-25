# cuts.dilang.tech production record

The public registry is live at **https://cuts.dilang.tech** as of 2026-09-23.
The corrected A record is **142.93.192.149**. External health and catalog
requests succeeded. Since 2026-09-25 nginx terminates TLS with certbot
certificates (see "Proxy and certificates" below); Caddy was retired.

Runtime revision: `3b5658611c1299a0f0197e1e8e0b300bb48a65ca` (v0.19.1, deployed
2026-10-24 with `tools/deploy_registry.sh` after a verified snapshot; 18 public cuts
re-verified). v0.19.0 (`961519af`) was deployed 2026-10-23 after verified snapshots
and Ubuntu HTTP/nginx/systemd staging checks.
The installed unit now starts `app.d6`; the upgrade preserves and restores the
previous unit as well as the release symlink on failure. All 19 new public cut
identities and all 18 retained legacy seed identities passed resolution, digest,
load, and locked-reinstall verification. See the 0.19 publication record below.

Previously `80810745f97a1ff27c1943515b6851117700afd1` (main, deployed
2026-10-19 with `tools/deploy_registry.sh` after a verified backup; the eleven cuts published on 2026-10-18
and graphsql 0.1.4 were already live, and `tools/verify_registry_launch.di` passed for all 18 cuts);
previously `bb35699e95bd3b9c8521d61e0d85e32417c59009` (v0.18.0, deployed
2026-10-18 with `tools/deploy_registry.sh` after a verified backup; the gremlin 0.4.4, graphql 0.1.6 and
mjolnir 0.2.3 cuts had been published earlier, and `tools/verify_registry_launch.di` passed for all 18 cuts);
previously `c9622650766b2f732693003efa7b2c2ce8ee6b3d` (v0.16.0, deployed
2026-10-16 the same way, no cut published);
previously `7f76de9d6e9c8397a0796e377e2da3610a2fc17d` (v0.14.0, deployed 2026-10-14 the same way);
previously `54c37ee66475dfbefde4e3620891f69d7532fdef` (main, deployed
2026-10-12 with `tools/deploy_registry.sh` after a verified backup; the bundled seed pins
`arel` 0.34.5, which was published the same day with a 30-minute credential (revoked,
archive sha256 `323edea3234d63777acf49fd57eb93ffe159688cb051e64e3596c2b228c92acf`), after
which `tools/verify_registry_launch.di` passed for all 18 cuts);
previously `8d64ab965949defcec3c9a0e066a79b2849d90b5` (main, deployed
2026-10-08 with `tools/deploy_registry.sh --no-verify` after a verified backup; it
carries the cut-card README previews and `arel` 0.34.4 in the bundled seed; `arel` 0.34.4
and `mjolnir` 0.1.0 were published the same day with a 30-minute credential (revoked); `mjolnir` 0.2.0 followed the same day the same way (archive sha256 `26614367548c192da3ca5c133aa5f58c914cc374b48ce3ddd7ab01e404caf58b`);
previously `6dfb7dae` (the `versions` merge
sort and the 18 README patch releases, published the same day), `8441e05c` from 2026-10-06, `c7eb036b`, `be0eb0ca` for 0.10.2,
`d951b2ac` for 0.10.1, `a4eabf76` for 0.9.2 and `26758cbfdd516860c21944d4bbfbcb2321947f18`
from 2026-09-25 for cut show pages;
earlier `822114807fbc9cbdd579f29fec4e0958da8b78df` from
2026-09-24, launch revision `f90c10bd`), built in the local Ubuntu 26.04 QEMU guest with x86-64-v3 release
flags. Only the current expanded release is kept under `releases/`; verified rollback archives are retained with the backups. Schema
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
and `verify_registry_launch.di` reinstalled all 18 cuts. Outage was about 30 s.

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
the current releases; use a newer facet. `verify_registry_launch.di` passed. No retained cut depends
on the removed cuts. Takedowns hide metadata and archive downloads while retaining
internal audit records and immutable version tombstones; those exact versions
cannot be republished. The source packages remain in the repository.
The publication and takedown credentials were temporary and have been revoked.
Initial published ownership belongs to `diamond-language`.

External HTTPS verification confirmed exactly 18 catalog releases, 404 responses
for each removed release's metadata and archive, and successful facet resolution,
digest checks, loading, and locked reinstall of every retained cut. The revised
seed also passed local reproducibility and dependency-closure checks.

## 0.19.0 publication, 2026-10-23

The [v0.19.0 release](https://github.com/diamond-language/diamond/releases/tag/v0.19.0)
tags `961519af64c36a27a3efccbea5d71248e7678e28`, tested by all 29 checks in
PR #301. It updates the 19 already-public cut names, without adding another
public name. The exact versions, dependency ranges, sizes, and SHA-256 digests
are frozen in [registry-0.19-inventory.json](../../../docs/registry-0.19-inventory.json).
That inventory is the 18-cut seed plus Mjolnir 0.3.1. Cancellation remains
source-only; the six withdrawn cuts remain unavailable.

Each missing version was checked before publication. Credential **29** had
only the 19 name-specific `publish:` scopes and a 30-minute lifetime. It stayed
on the registry host, was revoked after publication, and its secret file and
temporary publication payload were removed. Database revocation was verified.
The public catalog contains exactly the reviewed 19 latest identities. The new
cohort and the old 18-cut seed both resolved, matched their reviewed hashes,
loaded, and reinstalled from unchanged locks. Fresh Mjolnir 0.3.1/Arel 0.35.0
and legacy Mjolnir 0.2.4/Arel 0.34.6 installs passed the website's changeset,
query, association, and transaction example.

Pre-change database/blob snapshots were restore-verified on the host before
the registry upgrade; the Skindicate database snapshot passed SQLite integrity
and nonempty-data checks. Runtime binaries, static assets, nginx configuration,
and service units were retained in `/root/release-019-backup-20261023`.
After explicit operator approval, these snapshots and rollback files were
copied outside the Git worktrees into a private mode-0700 `.release-019/`
directory. The pre-change and post-release registry snapshots were restored
and verified locally; the downloaded Skindicate snapshot passed gzip and
SQLite integrity checks. The previous Skindicate binary was also retained
locally and matched the original running executable's SHA-256.

The registry deployment dry run completed before the upgrade. The Ubuntu
staging guest passed HTTP publication/install, nginx HTTPS/rate-limit/recovery,
and systemd identity/sandbox/restore checks. Production health, the `.d6`
service command, runtime version, and running executable identity were checked
afterward. The local build VM was shut down after release verification.

## Publishing a new cut version

Seed-time inventory lives in `docs/registry-launch-inventory.json`; a cut that
changes after launch is published on its own. Take and verify a laptop snapshot
first (`tools/fetch_registry_backup.sh`, below). Then:

1. On the host, issue a short-lived credential. **The subject must be a current
   owner of the cut** (`diamond-language` for every launch cut); a credential
   with any other subject gets HTTP 403 even with the right `publish:<name>`
   scope.

   ```sh
   # The umask inside the quotes applies on the server; the one around the ssh command is what makes the local file private.
   (umask 077 && ssh root@dilang.tech 'cd /opt/diamond-registry/current/app && umask 077 && \
     REGISTRY_ROOT=/var/lib/diamond-registry REGISTRY_OPERATOR=<you> \
     runuser -u diamond-registry -- ../bin/diamond credentials.d6 issue \
       diamond-language 1800 "publish:<name>" "<reason>"' > credential.json)
   ```

2. Prepare the seed (`build/diamond tools/prepare_registry_seed.d6 DIR --expect
   docs/registry-launch-inventory.json`) and trim it to the cuts being published with
   `tools/trim_seed_to.sh DIR NEW_SEED CUT...`, which checks each archive against its
   inventory entry; `tools/publish_registry_seed.d6` publishes every cut in the seed it
   is given. Never publish the whole seed again: versions are immutable.
3. Verify with `tools/verify_registry_launch.d6 https://cuts.dilang.tech`, delete
   `credential.json`, and revoke the credential
   (`credentials.d6 revoke <id> "<reason>"`) or let it expire.

gremlin 0.4.0 was published this way on 2026-10-02 and 0.4.2 (a raising handler answers 500)
on 2026-10-17 with a 30-minute credential (revoked; archive sha256
`8cebe7161bc9040642859bd8eebd3d761ca4729918f109b90ca0c123c2dbe782`), after which
`tools/verify_registry_launch.di` passed for all 18 cuts. `div` 0.2.4 (`Div.check_fresh`) followed the same day the same way (archive sha256
`474ba288bae1b16cf8a8799b8b3fe60e9a97087c8a578ad279d19437e0be3872`, credential revoked, verification passed). `gremlin` 0.4.3 (`request["peer_address"]`, sha256
`47b899ebc0e5ea0ca1797fd1874883831207f11e06f1f5e27fe0ae707aea70a1`) and `graphql` 0.1.5 (`Schema#to_sdl`, sha256
`522ecb0e961c9522b4689324536b60746277d1e9ab40a60d754e9313030d90f3`) were published later the same day the same way,
each with its own revoked credential and a passing verification. `mjolnir` 0.2.1 (composite `unique_constraint`, sha256
`bd98ce4a26d8f3405fedd181eaa13166da836f0aaad425b3a0ddc0ad336e18a3`) followed the same way (its seed was prepared with `--selection` naming `arel` and `mjolnir`, then trimmed to mjolnir). `gremlin` 0.4.4 (the shutdown-race fix, sha256
`3193e94ba166304acca80014e79bacc6dc1a2723314df3c507534f25b5d3005b`) followed the same way, with a passing verification of all 18 launch cuts. `graphql` 0.1.6 (`GraphQL::ExecutionError` extensions, sha256
`d4bd03cd6210591b0e42d2b5c36da3fb6eb81b56e8329aab4cb51002bb1c7ed8`) followed the same way (credential 21, revoked), with a passing verification of all 18 launch cuts. `mjolnir` 0.2.2 (composite primary keys, sha256
`abd5244bd466bbe0e0886384c7b3e37a1a8fd95b0950f40596b0bc7206860618`, credential 22) and 0.2.3 (Hash-row updates and `BEGIN IMMEDIATE` transactions, sha256
`3f845a09c545c320840703faa38c3e7b19863a57d6b00d2b2a950c5c6af21987`, credential 23) followed the same way, each credential revoked and each followed by a passing verification. `graphql` 0.1.7 (parser nesting cap, fragments on introspection types, fragment-cycle validation, sha256
`e8109c37cbbec5ecf5eb31ec2358b47bbfea60a8266bf6ab501bbc09bd770933`, credential 24) and `gremlin` 0.4.5 (a handler's stack overflow answers 500, sha256
`536c6072f05b1e41aefc95dc6f921d391f55abd5ca7e546d675f6d549e2ee3bf`, credential 25) followed the same way on 2026-10-18, both credentials revoked, with a passing verification of all 18 launch cuts.
`gremlin` 0.4.6 (the `host` argument that binds a listener to one address, sha256
`5e40bd79a0ecd124f7022a686bc2c6bfc6c711d9398562256dbfa889c71092fc`, credential 26) followed the same way on 2026-10-18, revoked, with a passing verification of all 18 launch cuts.

On 2026-10-19 eleven cuts were published together after a verified laptop snapshot, with one 30-minute credential (credential 27, `publish:<name>` for each of the eleven, revoked afterwards): `arel` 0.34.6 (sha256 `b485d8a5001baac2b790b2a43912cb0bba74b3bfce091d7cc23013291d4af43f`), `active_record` 0.19.5 (sha256 `7db236e823af5a6d73def227a5ba908fe8a985cd0f184b55bcbd9dfacff41946`), `graphql` 0.2.1 (sha256 `d6f62865ff51bcdcfef18d70f5cdb3c2559c49e2bcc2c96648eb403f22c26bf8`), `http` 0.1.3 (sha256 `453d37182c35d0deb17166a7b59c39c8716db7dc054c6fe740c995f2d119fc7e`), `gremlin` 0.4.7 (sha256 `95e2548f5b7f391209fd471dee68c4bba00944aa83fe8d21db31f28c6ee12fd9`), `jobs` 0.1.3 (sha256 `7d0a284a03c4b4d7486f4f866c3befe614c14fabbe3126293a61c2e361146d4f`), `network_safety` 0.1.3 (sha256 `9fb21cae5568472003d2036364dfa988b2c9ce4b436f463d1e9064f75161d923`), `rack` 0.1.3 (sha256 `f2bd03fb212241406c3676d1bb97bb9790a851f473d145c0760b88a38b92ad34`), `redis` 0.1.3 (sha256 `cb0c9486f265926e20d258fdb09710fc576ebc722fa2ad00a841ddc66958490e`), `websocket` 0.1.4 (sha256 `7186d31b7918382b36aba04a5ed711cc66505c962987a9381c65725aeaaffaa7`), `mjolnir` 0.2.4 (sha256 `8df73f569a146e0d1c122f9f012705dbdac1e0c4a10c3d25fa71fe521144a831`). They carry the review fixes: the `graphql` strictness and query limits, the `gremlin` per-request connection, the `rack`, `http`, `websocket`, `redis`, `jobs` and `network_safety` hardening, the `arel` bind-order fix, and the `mjolnir` MySQL/MariaDB writes. `graphql` 0.2.x is a breaking line for `graphsql` 0.1.3, whose range stops at `graphql` 0.1.x, so `tools/verify_registry_launch.di` failed on the mismatch until `graphsql` 0.1.4 (which depends on `graphql ^0.2.0`) followed.
`graphsql` 0.1.4 (sha256 `7a1d5253251075d2b70deb048b2bb6c09e2b8f468000037e01c0abe01b9a172d`, credential 28, `publish:graphsql`, revoked) followed the same day, after which `tools/verify_registry_launch.di` passed for all 18 launch cuts. The registry service was not redeployed for these publications and at that point still served from release `bb35699e`.

`minitest` 0.1.0, a new cut (the minitest library as a development dependency), was published on 2026-10-25 the same way after a verified laptop snapshot (sha256
`6e7976ef0ce8e13403861b7219ad871211dee0bec7f65ac40d84b5b1063a89f5`, credential 30, `publish:minitest`, revoked), after which `tools/verify_registry_launch.d6` passed for all 19 launch cuts.

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
was downloaded to the operator-selected `~/diamond-registry-backups/<dated-directory>/snapshot`
and verified by a temporary local restore on 2026-09-23T18:42:37+00:00.
Database integrity, foreign keys, archive sizes and digests passed. The snapshot
contains 18 public releases and six retained takedowns (24 archive blobs total);
its parent directory is private (0700). Future downloads require a new directory.
Automatic alerts are explicitly deferred; no receiver or automatic off-host
backup schedule is configured. Do not represent
scheduled backups or automatic alert delivery as operational. Recovery can lose
all changes since the last manual snapshot; record when each verified copy was
made and repeat before upgrades, credential changes, and package publication.

Keep backups **outside every source checkout** (the convention is
`~/diamond-registry-backups/`, mode 0700, one dated directory per snapshot) so
they can never be committed or shipped by accident, and keep only the latest
verified snapshot once a newer one has passed restore verification.

On the laptop, select a new private destination yourself and use:

```sh
tools/fetch_registry_backup.sh /your/chosen/new-backup-directory \
  --host root@dilang.tech \
  --app /opt/diamond-registry/current/app \
  --data /var/lib/diamond-registry
```

Replace the application directory with the installed path containing `backup.di`.
The helper uses existing SSH keys and strict host-key verification. It snapshots
the running registry into a temporary directory on the server, streams the copy
over SSH, checks that it holds only plain files below `snapshot/` (no links, no absolute or
`..` names), extracts it, and performs a local restore verification with `backup.di`. It reports success only after database/blob checks pass.
The server temporary copy and local verification copy are removed; the retained
snapshot includes credential digests and must stay private. The destination must
not exist, and a failed transfer removes only the newly created destination.
No transfer is made until an operator supplies the destination and remote paths.

To rehearse recovery without touching a running service:

```sh
diamond applications/registry/backup.d6 restore \
  /your/chosen/backup-directory/snapshot /your/chosen/new-recovery-directory
```

Then run the recovered registry in QEMU, inspect release and credential state,
and perform a facet installation. Follow the service restore procedure when
switching real traffic. Manually run `monitor.di https://cuts.dilang.tech` until
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

For an explicitly approved new seed, use `tools/publish_registry_seed.di` with the
HTTPS registry URL, `--seed <verified-seed-directory>`, and
`--credentials <private-credential-json>`. Issue short-lived name-scoped credentials
with `credentials.di`; revoke them after the operation and remove the private file.
The publisher verifies archive identities and respects the live write rate limit.
Do not reuse the original deployed 24-cut seed: use the current 18-cut selection.

Run `build/diamond tools/verify_registry_launch.di https://cuts.dilang.tech` to resolve
all selected cuts into a temporary clean consumer, compare the lock against the
reviewed inventory, load every cut, and reinstall from the unchanged lockfile.
