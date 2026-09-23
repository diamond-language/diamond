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
credential administration and owner-management commands are still pending.

Endpoints relative to the configured base:

- `GET /health`
- `GET /v1/cuts/<name>/versions`
- `GET /v1/cuts/<name>/versions/<version>`
- `GET /v1/blobs/sha256/<digest>`
- `POST /v1/cuts/<name>/versions`

Only committed releases are served. Yanking preserves direct metadata and blob
access; takedowns hide both. Indexes use SemVer precedence, including numeric
prerelease identifiers. Errors return protocol JSON without internal exception
messages. Responses currently use `Cache-Control: no-store` so later takedowns
are not hidden by HTTP caching.

This first service uses one worker and buffers request bodies and blobs. The
current HTTP parser imposes a 25 MiB upload limit, below the artifact verifier's
56 MiB limit, and closes connections exceeding it. Configure the proxy to reject
oversized uploads with HTTP 413. Streaming uploads, rate limits, structured
request logging, administrative endpoints, backups, and production deployment
remain follow-up work.

Run `make test-registry-http` from the repository root for the live integration
suite. It uses Python 3, OpenSSL, real curl, a temporary CA trusted only by the
test, a local HTTPS proxy, and a running Diamond service. It publishes cuts with
transitive dependencies, resolves and executes installed code, reinstalls a
yanked locked release, and checks takedown visibility.
