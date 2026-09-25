# Cut registry service

A single-node HTTP application using the `registry` cut and Gremlin. It serves
version indexes, release metadata, verified blobs, and authenticated publishing.
The API is independent of a web interface. See [the protocol](../../docs/registry-protocol.md).

From the repository root, build the tools and install the local cuts:

```sh
make
make facet
tools/install_local_cuts.sh applications/registry
cd applications/registry
umask 077
mkdir -p data/blobs data/staging
export REGISTRY_ROOT="$PWD/data"
export REGISTRY_FACET="$(cd ../.. && pwd)/build/facet"
export REGISTRY_PORT=18120
../../build/diamond app.di
```

Schema migration runs at startup. `REGISTRY_ROOT` must point to private storage
owned by the service account; it holds `registry.db`, `blobs/`, and `staging/`.
`REGISTRY_FACET` must identify a trusted executable. Optional `REGISTRY_BASE`
sets an external base path such as `/registry` (default empty, no trailing slash).
The proxy must preserve that prefix when forwarding requests.

Terminate HTTPS at a reverse proxy. Gremlin listens on all interfaces, so the
backend port must be reachable only by the proxy in a hosted deployment. Reads
are public. Provision random-token credentials and name scopes as described in
the [registry package](../../packages/registry/README.md) before publishing;
use the local credential commands below. Owner management is available over
the authenticated API.

Endpoints relative to the configured base:

- `GET /health`
- `GET /v1/cuts/<name>/versions`
- `GET /v1/cuts/<name>/versions/<version>`
- `GET /v1/blobs/sha256/<digest>`
- `POST /v1/cuts/<name>/versions`
- `POST /v1/cuts/<name>/versions/<version>/yank`
- `POST /v1/cuts/<name>/versions/<version>/unyank`
- `POST /v1/cuts/<name>/versions/<version>/takedown`
- `GET /v1/cuts/<name>/owners`
- `POST /v1/cuts/<name>/owners/add`
- `POST /v1/cuts/<name>/owners/remove`
- `GET /v1/audit?after=0&limit=50`

Only committed releases are served. Yanking preserves direct metadata and blob
access; takedowns hide both. Indexes use SemVer precedence, including numeric
prerelease identifiers. Errors return protocol JSON without internal exception
messages. Responses currently use `Cache-Control: no-store` so later takedowns
are not hidden by HTTP caching.

This service uses one worker and buffers request bodies and blobs. Bounded
parsing rejects oversized bodies with HTTP 413 and oversized lines, headers, or
header counts with HTTP 431, using protocol JSON errors and request IDs.
Malformed or ambiguous framing (including duplicate headers, negative/noncanonical
lengths, and transfer encoding) returns HTTP 400. `Expect: 100-continue` is
supported after size validation; other expectations return HTTP 417.

Defaults and startup settings:

| Limit | Default | Configuration |
| --- | --- | --- |
| Body bytes | 25 MiB | `REGISTRY_MAX_BODY_BYTES`, 1–56 MiB |
| Active connections | 8 | `REGISTRY_MAX_CONNECTIONS`, 1–1024 |
| Connection I/O deadline | 30 seconds | `REGISTRY_TIMEOUT_SECONDS`, 1–3600 |
| Request/header line including CRLF | 8192 bytes | Fixed |
| Combined request line and headers | 32768 bytes | Fixed |
| Header count | 100 | Fixed |

Expired connections and connections above the capacity limit are closed, with
structured log events; these closures do not promise an HTTP error response.
The deadline starts at accept and does not reset when bytes arrive. It bounds
stalled socket reads/writes while the worker polls; it cannot interrupt
synchronous database, verifier, or handler work. Upload memory consumption grows
with the body limit and connection count, with additional buffer copies.
Streaming uploads, rate limits, backups, and production deployment remain.

Logs are newline-delimited JSON. Handled requests log `request.started` and
`request.completed` with a generated request ID, method, sizes, handler duration,
and status. The same ID appears in `X-Request-ID` and JSON error bodies. Early
parser rejections and I/O deadlines are logged by Gremlin. URLs, queries,
headers, tokens, and bodies are excluded from registry request logs. Completion
means the handler produced its response; it does not confirm delivery to the
client. Configure the reverse proxy's limits and deadlines consistently.

Run `make test-registry-http` from the repository root for the live integration
suite. It uses Python 3, OpenSSL, real curl, a temporary CA trusted only by the
test, a local HTTPS proxy, and a running Diamond service. It publishes cuts with
transitive dependencies, resolves and executes installed code, reinstalls a
yanked locked release, and checks takedown visibility.

## Request maintainership

Developers can [request public-registry maintainership through GitHub issues](../../docs/registry-maintainership.md).
Use the template for new publications, additional maintainers, and ownership
transfers. Operators verify source ownership and deliver scoped credentials privately.

## Local credential administration

Run these commands from the application directory, after the application has
initialized the database. Access is controlled by local database filesystem
permissions. `REGISTRY_OPERATOR` identifies the operator in audit events;
it is an operator-supplied label, not an authentication mechanism.

```sh
export REGISTRY_OPERATOR="operator@example.com"
umask 077
../../build/diamond credentials.di issue alice 3600 'publish:greeter,manage:greeter' 'initial access' > credential.json
../../build/diamond credentials.di list
../../build/diamond credentials.di rotate 1 3600 'routine rotation' > replacement.json
../../build/diamond credentials.di revoke 2 'access removed'
```

Use the actual IDs returned by your commands. Issuance and rotation print the
random 256-bit token once as JSON on stdout; keep that output private. Only its
SHA-256 digest is stored. Lifetime is required, between 1 second and 365 days.
Allowed scopes are `publish:<name>`, `manage:<name>`, and `admin`. Inventory omits
both raw tokens and token digests. Rotation preserves subject and scopes and
revokes the old credential in the same transaction that creates the new one.
Credential creation/revocation and their audit records commit together.

Release administration accepts `Content-Type: application/json` and an object
with exactly one field, `reason`, containing 1–1024 bytes of nonblank text.
Requests require a bearer credential. Yank and unyank require `manage:<name>`
and ownership; takedown requires `admin`. Successful yank/unyank return the
complete release record. Takedown returns name, version, protocol, and
`taken_down: true`. Retries preserve the original reason and do not add duplicate
state-change audit events. Takedown is permanent through this API: unyank cannot
restore it, and the version and archive identity remain reserved.

## Ownership and audit inspection

Owner-list reads and changes require either an administrator credential or a
current owner's `manage:<name>` credential. Add/remove requests contain exactly
`owner` and `reason` in a JSON body. Subjects must match credential subjects;
adding ownership does not issue a credential. Removal immediately affects both
publishing and management. The final owner cannot be removed, even by an
administrator. Changes commit with their audit events and repeated unchanged
requests do not add events.

Administrators can inspect `/v1/audit` using `after` and `limit` query parameters.
The default page size is 50 and maximum is 100. Follow `next_after` until null.
Events include actor, affected owner, reason, release identity, credential ID,
and scope/expiry snapshots where applicable. No raw tokens or token digests are
exposed. Credential events distinguish the local operator (subject) from the
affected credential (credential ID). See the protocol for complete response
fields and authorization rules.

## Backup and recovery

Use Python 3's standard library tool with trusted local source and destination
parents. Run as the storage owner. Both commands require a **new destination**:

```sh
python3 backup.py backup /var/lib/diamond-registry /srv/backups/registry-20260923
python3 backup.py restore /srv/backups/registry-20260923 /srv/recovery/registry
```

Backup may run while the service publishes. SQLite's backup API captures a
consistent database; immutable blobs referenced by that snapshot are then copied
and verified. This relies on the current policy of retaining blobs, including
taken-down releases. Do not delete blobs or run garbage collection during backup.
Orphan blobs and staging uploads are excluded. The snapshot includes credentials,
owners, audit events, idempotency keys, and release state. Protect and encrypt
backup storage as you would the live database; credential digests remain sensitive.

`manifest.json` records the database checksum and complete blob inventory and is
written last. Restore checks those values, SQLite integrity and foreign keys,
and every blob's digest and size. Checksums detect damage; they do not authenticate
a backup against malicious replacement. Files are synced before success is
reported. Failed operations remove their new destination; process termination or
power loss can leave a partial directory. Never use a partial restore. A backup
without a valid manifest cannot be restored. Keep successful snapshots off-host
and establish retention and periodic recovery drills with your operator.

Restore does not alter the running service. After a successful restore, stop the
service, point `REGISTRY_ROOT` and its service write permissions at the recovered
directory, and restart. Check health, authentication, release state, and a real
facet installation before reopening traffic. Credential expiry times are preserved;
restoring an older snapshot also restores its older revocation and ownership state.
Reconcile changes since that snapshot before reopening writes. The HTTPS test runs
an online backup, verifies database contents, restarts on recovered data, and
installs and executes a cut through facet.

## Service deployment template

`deploy/registry.service` and `deploy/registry.env.example` provide a Linux systemd
starting point. Install built binaries and locally installed cuts under
`/opt/diamond`, create the `diamond-registry` service account, and install the
environment file as `/etc/diamond-registry.env`. Create
`/var/lib/diamond-registry/{blobs,staging}` owned by that account with mode 0700
before starting. Install the unit in `/etc/systemd/system/`, reload systemd, and
enable/start `registry.service`. Adjust paths in both files together. Application
code should be read-only to the service account; only its state directory is writable.
Logs go to the journal. The template is not an automatic deployment.

Configure your HTTPS proxy to preserve the path prefix, allow the configured
upload size, and use timeouts appropriate to archive verification. Restrict port
18120 to the proxy with host/network firewall rules **before** starting the service:
Gremlin binds all interfaces. Configure TLS certificates, proxy rate limits,
monitoring, and backup scheduling for your host. No public service, proxy, firewall,
or certificate configuration is installed by this repository.

## Proxy limits and monitoring

See [deployment operations](deploy/OPERATIONS.md) for the nginx rate-limit template,
HTTPS health/archive probe, alert signals, and launch and upgrade checklists.
Templates require host-specific configuration and validation before deployment.

## Public catalog and onboarding

The intended public endpoint is `https://cuts.dilang.tech`. The application root
serves a searchable release catalog and getting-started instructions. The browser
loads live metadata from `catalog.json`; taken-down releases are excluded and
yanked versions are marked. The catalog lists one row per cut: its newest
unyanked release by SemVer precedence, or its newest release if every version
is yanked. Each row carries the release's `maintainers` as a JSON string, or
`null` for releases published before manifests declared them. Pages contain at
most 100 cuts, with `next_after` as an ascending cut-ID cursor. Search filters the loaded releases; load more
to search additional pages. The catalog uses text rendering for metadata and a
same-origin script policy. It contains no publishing credential or administration UI.

Keep `catalog.di`, `catalog.html`, `catalog.js`, `catalog.css`, `cut.html`, and
`cut.js` beside `app.di` when deploying. `/cuts/<name>` is a cut's show page
(README, install command, and version history), backed by `catalog/<name>.json`.
`REGISTRY_BASE` prefixes the catalog and its assets as well as the API. Install
commands use the catalog's origin/base; the public quickstart uses cuts.dilang.tech.
See [production preparation](deploy/PRODUCTION.md) for the nginx host
integration and unresolved launch inputs. Providing the hostname in docs does not
mean the public service is deployed.
