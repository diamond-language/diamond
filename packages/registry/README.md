# registry

Storage primitives for the Diamond cut registry.

The package provides the SQLite schema and a content-addressed blob store.
The authenticated publish transaction verifies archives with `facet` and stores
release metadata with its audit event. The HTTP adapter is available as
`Registry::API`; a runnable service lives in `applications/registry`. Local credential commands are documented with the application.

Install with `facet` and load with `require_cut "registry"`.

`BlobStore.read` verifies the stored bytes against their digest and raises
`IOError` for corrupt or incomplete files. `contains?` returns false when the
file is missing, unreadable, or fails verification. A duplicate `put` raises
`IOError` without replacing the existing file.

Writes use `File.publish`: synchronize a private temporary file, atomically
link it to the digest path without replacement, remove the temporary name,
and synchronize the containing directory. The root must be an existing,
trusted directory on a local filesystem supporting hard links and directory
synchronization. Blob files are private to the service account (mode 0600).

A crash before publication can leave `.diamond-publish-*` staging files;
these are ignored by digest lookup and may be removed when writers are stopped.
A failure after linking can leave a complete published file even though the
call raises. Recovery must verify that file before recording a release or
retrying. The publisher performs this recovery check automatically.

Run `make test-registry-package` from the repository root. The tests verify
expected exception types, unchanged bytes after a duplicate write, invalid
digest rejection, and corrupt-file detection.

## Publish transaction

Create a `Registry::Publisher` with an initialized SQLite connection, a
`BlobStore`, an existing private staging directory, and the trusted absolute
path to the `facet` executable. Run schema migration once during startup,
before opening worker connections. Each worker must use its own connection.

```diamond
publisher = Registry::Publisher.new(db, store, staging_path, facet_path)
result = publisher.publish(name, archive_bytes, bearer_token, idempotency_key)
```

The optional key defaults to an empty String. The result includes protocol,
name, version, sha256, size, yanked, and an internal `created` flag. An HTTP
adapter should remove `created` and use it to choose status 201 or 200.

Operator-provisioned credentials store the SHA-256 digest of a cryptographically
random token, a subject, a JSON array of scopes, and optional expiry/revocation
timestamps. Use at least 256 random bits for tokens. `publish:<name>` is required;
for an existing cut the subject must also be an owner. The first successful
publish claims an unused name for that subject and audits the claim.

Publishing uses a bounded in-memory archive (at most 56 MiB), verified via
`facet verify --sha256 ... --json`. It never executes package code. An immediate
SQLite transaction serializes authorization, claims, releases, audit records,
and idempotency keys. The blob is synchronized before the transaction commits.
Repeated identical uploads return the existing release without extra publish
audit events or changing yank status. Takedown tombstones cannot be republished.

A failed database operation leaves no partial metadata. A complete orphan blob
may remain and is verified and synchronized on retry. Corrupt existing blobs
fail closed and need operator repair. No automatic blob garbage collection is
provided. SQLite `synchronous=FULL` is configured; this single-node implementation
requires local durable storage and holds the writer lock during verification.
Authentication errors use `RuntimeError` codes, invalid requests/archives use
`ArgumentError` codes, and filesystem failures propagate as `IOError`; the
HTTP adapter maps these to protocol responses.

## HTTP adapter

`Registry::API.new(db, store, publisher, base_path)` accepts the same initialized
connection and storage objects as the publisher. `base_path` defaults to empty.
`call(request)` returns a Rack-style `[status, headers, body]` response. Request
headers must be normalized to lowercase, as the HTTP parser does. A service
worker keeps its own API instance and SQLite connection.

The adapter exposes version indexes, release metadata, digest-addressed blobs,
publishing, and audited yank/unyank/takedown operations. It omits the publisher's internal `created` flag from JSON.
Only releases with no takedown reason expose blobs; orphan files are never
served. A storage integrity failure returns an internal error without bytes.
Run `make test-registry-http` to exercise the adapter through real HTTPS and
facet, including SemVer ordering and transitive dependency installation.

`Registry::Administration` implements credential issuance/revocation and release
state changes. Release mutations authenticate inside an immediate transaction
and atomically record the credential, reason, release identity, and digest.
Idempotent retries do not duplicate audit events. For local credential events,
`subject` is the operator label and `credential_id` identifies the affected
credential; no raw credential is recorded. The application CLI offers issue,
list, rotate, and revoke commands under filesystem access control.
