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
use the local credential commands below. Owner-management commands remain pending.

Endpoints relative to the configured base:

- `GET /health`
- `GET /v1/cuts/<name>/versions`
- `GET /v1/cuts/<name>/versions/<version>`
- `GET /v1/blobs/sha256/<digest>`
- `POST /v1/cuts/<name>/versions`
- `POST /v1/cuts/<name>/versions/<version>/yank`
- `POST /v1/cuts/<name>/versions/<version>/unyank`
- `POST /v1/cuts/<name>/versions/<version>/takedown`

Only committed releases are served. Yanking preserves direct metadata and blob
access; takedowns hide both. Indexes use SemVer precedence, including numeric
prerelease identifiers. Errors return protocol JSON without internal exception
messages. Responses currently use `Cache-Control: no-store` so later takedowns
are not hidden by HTTP caching.

This first service uses one worker and buffers request bodies and blobs. The
current HTTP parser imposes a 25 MiB upload limit, below the artifact verifier's
56 MiB limit, and closes connections exceeding it. Configure the proxy to reject
oversized uploads with HTTP 413. Streaming uploads, rate limits, structured
request logging, owner management, backups, and production deployment
remain follow-up work.

Run `make test-registry-http` from the repository root for the live integration
suite. It uses Python 3, OpenSSL, real curl, a temporary CA trusted only by the
test, a local HTTPS proxy, and a running Diamond service. It publishes cuts with
transitive dependencies, resolves and executes installed code, reinstalls a
yanked locked release, and checks takedown visibility.

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
