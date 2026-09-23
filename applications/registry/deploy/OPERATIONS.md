# Registry deployment and operations

## Proxy configuration

Use `nginx.conf.example` in the proxy's `http` context on a dedicated hostname.
Replace the hostname and certificate paths. The example assumes an empty
`REGISTRY_BASE`; a nonempty base must match the application's public URL. Forward
that prefix unchanged. Run `nginx -t` on the deployment host before reloading.
The template is not installed automatically.

The initial policy allows 20 requests/second per source IP with a burst of 40,
and additionally limits writes to 6/minute with a burst of 3. Excess requests
receive 429. Tune these starting values with observed load and shared NAT clients.
The write limit applies before authentication and therefore also covers invalid
credentials. Request buffering prevents slow uploads from occupying a backend
connection while the proxy receives their bodies. Proxy temporary storage must
have capacity for concurrent uploads and be private to its account.

These settings follow nginx's [rate limit module](https://nginx.org/en/docs/http/ngx_http_limit_req_module.html)
and [proxy module](https://nginx.org/en/docs/http/ngx_http_proxy_module.html).
The read zone counts all requests; the additional write zone uses an empty key
for GET/HEAD. Both limits apply in the same location. Client identity comes from
the TCP peer, not an untrusted forwarded header. If a load balancer precedes nginx,
configure trusted real-IP handling for its exact network before using this policy.

Keep the 25 MiB proxy body limit aligned with `REGISTRY_MAX_BODY_BYTES=26214400`.
Proxy 429/413 and upstream failures can be nginx HTML responses without registry
request IDs. Successful upstream responses retain the registry's `X-Request-ID`.
Proxy access JSON logs omit URLs, headers, client addresses, and bodies. Nginx
error logs can include request URLs; restrict access and retention, never put
credentials in URLs, and do not enable debug logging on live traffic.

## Monitoring

Run the standard-library probe from outside the service host:

```sh
python3 applications/registry/monitor.py https://cuts.example.com
python3 applications/registry/monitor.py https://cuts.example.com --archive-sha256 DIGEST
```

For a private CA, use `--ca-file /path/to/ca.pem`. Certificate and hostname
verification stay enabled. Redirects are rejected. Exit 0 means success; exit 1
and a JSON `ok: false` record indicate failure. The output contains elapsed time
and an error class without endpoint or response contents. `--timeout` sets socket
inactivity timeout (default 10 seconds); use a scheduler process deadline, such as
`timeout 30s python3 ...`, to bound the whole job. A pinned archive check streams
and hashes up to the registry's 56 MiB maximum; choose a small retained canary cut.

Health checks run `SELECT 1`; they do not prove disk writability, blob integrity,
or publishing functionality. Run health each minute and a pinned archive check
less frequently. A takedown of the selected canary makes that check fail. Perform
periodic facet install/execute and publish drills in a separate staging registry.

Route these signals to your monitoring system and a named operator:

| Signal | Initial action |
| --- | --- |
| Three consecutive external probe failures | Page operator; inspect proxy, service status, and network |
| Sustained 5xx or rising request duration | Inspect application `request.completed` logs and proxy latency |
| Rising 429, `connection.rejected`, or `request.timeout` | Check load and abuse before raising limits |
| Low free bytes/inodes on data, proxy temporary, or backup volumes | Alert before writes fail; retain referenced blobs |
| Backup older than agreed recovery window, or failed restore drill | Page backup owner; retain last verified snapshot |
| Certificate approaching expiry or service restart loop | Repair renewal/startup and repeat external probe |

Aggregate proxy JSON status/duration/limit fields and application JSON events.
Application durations cover handler work; proxy durations also cover network
transfer. No metrics exporter or alert delivery service is bundled. Establish
thresholds, log rotation, retention, and an alert receiver in the host environment.

## Launch checklist

- Assign service, security/abuse, and backup owners with incident contacts.
- Install verified binaries and cuts as read-only application files; provision the
  private service account, state directories, and environment file.
- Restrict backend port 18120 to the proxy; verify from an external host that it
  cannot be reached. Test HTTPS certificates and the public base path.
- Validate systemd and nginx configuration on the host. In staging, test ordinary
  reads, uploads near the configured size limit, and bursts yielding 429; verify
  unauthenticated writes fail and logs do not disclose credentials.
- Provision scoped credentials with expiry and record their owners. Keep bootstrap
  operator access restricted; exercise rotation and revocation.
- Define recovery point/time targets and retention; schedule off-host backups and
  perform a restore drill with a real facet install before opening writes.
- Connect external probes and host/log alerts to the operator; trigger a test alert.
- Record takedown, ownership dispute, and credential compromise procedures. On an
  incident, restrict writes, preserve audit evidence, and rotate affected credentials.

For upgrades, take a verified backup, stage the new version and its migrations,
then restart and check health, auth, and a canary install. Do not assume old
binaries can read a migrated database. If rollback needs a snapshot, restore to a
new directory while traffic is closed, account for writes since the snapshot,
and reconcile credential revocations before reopening. See the application's
[backup and recovery instructions](../README.md#backup-and-recovery).

## Local QEMU staging gate

Staging means an isolated test deployment; this project prefers a local QEMU VM.
The existing Ubuntu 26.04 build guest can be reused without changing its normal
build checkout. Use QEMU user networking (NAT), forward SSH only on host loopback,
and do not forward the guest's registry ports. Install nginx and the repository's
Ubuntu build dependencies, Python 3, curl, and OpenSSL in the guest first.

From the host checkout:

```sh
REGISTRY_VM_PORT=2222 \
REGISTRY_VM_KNOWN_HOSTS=/path/to/verified/known_hosts \
  tools/test_registry_vm.sh
```

The runner uses `builder@127.0.0.1`, strict host-key verification, and existing SSH
keys. The default known-hosts file is `../.vm-build/known_hosts`, matching the local
build VM workflow. It transfers the current source snapshot (including pending
nonignored files), builds in a fresh `/tmp` directory, runs `test-registry-nginx`
and `test-registry-http`, and removes that directory afterward. Review untracked
files before running it. It neither changes the usual build checkout nor installs
packages automatically. The VM remains running afterward.

The nginx drill changes only addresses, ports, certificates, and log paths in the
shipped proxy template. It uses a temporary local certificate with verification
on, a temporary database and credential, and unprivileged nginx on guest loopback.
It checks real facet publishing/install/execution, preserved request IDs and base
paths, 401/413 rejection, independent write throttling, read throttling and recovery,
and access-log redaction. No real package is published. The separate HTTPS suite
also exercises administration and restoring a service from backup. Full systemd
startup, public DNS, certificate renewal, alert delivery, and host firewall checks
remain deployment gates.

Validated on 2026-09-23 in the local Ubuntu 26.04.1 QEMU guest with nginx
1.28.3: the nginx staging gate and the full registry HTTPS integration suite
passed, including monitored archive access and backup restoration.
