# Cut registry protocol, version 1

Status: proposed wire contract. Registry resolution and locked archive
installation are implemented; the hosted service is not yet implemented. This
specification defines its metadata, authentication, and artifact boundary
without changing Git dependency behavior.

## Identity

A release is identified by `(registry URL, cut name, version)`. The registry URL is a normalized HTTPS origin plus an optional base path, with no query or fragment. `facet` stores that exact configured URL in the lockfile and never substitutes another registry. A name follows the publishable cut name rule in [cut-contract.md](cut-contract.md). A version is canonical SemVer 2.0.0 without a leading `v` or build metadata. Each tuple is immutable, including its metadata and archive digest.

Protocol responses include `"protocol": 1`. Unknown protocol versions fail closed. Clients ignore only explicitly optional display fields; unknown fields in identity, dependency, or artifact records fail. Responses use UTF-8 JSON with bounded sizes. JSON object keys must be unique. Registry metadata is data: neither resolution nor installation executes code from a cut.

## Errors

Every failed request returns a JSON object with `Content-Type: application/json`:

```json
{
  "protocol": 1,
  "error": "release_exists",
  "message": "version 1.2.0 is already published",
  "request_id": "01J..."
}
```

`error` is a stable machine-readable code. `message` is for humans and is not
parsed by clients. `request_id` is optional for local clients and is included
by hosted deployments for support and audit correlation. The initial code set:

| Status | Code | Meaning |
| --- | --- | --- |
| 400 | `invalid_request` | Malformed path, header, or archive request |
| 401 | `unauthorized` | Missing or invalid credential |
| 403 | `forbidden` | Credential lacks the required scope |
| 404 | `not_found` | Cut, release, or blob does not exist |
| 409 | `release_exists` | Immutable release conflicts with this request |
| 409 | `idempotency_conflict` | Idempotency key is reused for another body |
| 413 | `payload_too_large` | Request exceeds the configured limit |
| 422 | `invalid_archive` | Archive fails cut or manifest validation |
| 429 | `rate_limited` | Caller must wait before retrying |
| 500 | `internal_error` | Server could not complete a valid request |
| 503 | `unavailable` | Service is temporarily unavailable |

Clients may retry `429`, `500`, and `503` with bounded backoff. They must not
retry a publish after `400`, `401`, `403`, `409`, or `422` without changing the
request.

## Authentication and ownership

Publish and administrative requests send `Authorization: Bearer <token>` over
HTTPS. Tokens are opaque, stored only as password-equivalent hashes, and never
returned by the API. A credential has scopes such as:

- `publish:<name>` to publish a release for one cut;
- `manage:<name>` to yank, restore, or manage owners for one cut;
- `admin` for exceptional takedown and credential administration.

Read endpoints are public by default. Token creation, rotation, and revocation
are operator actions in v1. Servers reject credentials containing control
characters and record credential identity, scopes, and expiry in audit events,
never the token value.

## Read API

All paths below are relative to the configured registry base. Path segments are percent-encoded from validated ASCII names and versions. Responses must use `Content-Type: application/json` and may use `ETag` and `Cache-Control`; clients may cache them, but a lockfile install does not query the version index.

### `GET /v1/cuts/<name>/versions`

Returns an object containing `"protocol": 1` and a `versions` array, sorted by
SemVer precedence and then by canonical version text. Each version record has
exactly `version` and `yanked`; the release metadata URL follows the fixed path
below. The index can omit yanked releases for new resolution, but a direct
release lookup must still work for a locked release unless an exceptional
takedown occurred. The client treats array order as presentation only and
selects deterministically itself.

### `GET /v1/cuts/<name>/versions/<version>`

Returns a release record:

```json
{
  "protocol": 1,
  "name": "greeter",
  "version": "1.2.0",
  "dependencies": {"logger": "^0.4.0"},
  "yanked": false,
  "archive": {
    "path": "/v1/blobs/sha256/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "size": 12345
  }
}
```

The record's name and version must match the request. Dependency names and ranges use the same validation as `diamond.cut`. The archive path stays under the configured registry base; redirects to another origin are rejected. The digest is 64 lowercase hexadecimal characters. The size is a positive integer within the archive limit. Optional display metadata is separate from resolution fields and never supplies a download URL.

### `GET /v1/blobs/sha256/<digest>`

Returns the exact, uncompressed tar bytes specified in [cut-contract.md](cut-contract.md), with `Content-Type: application/octet-stream` and a bounded `Content-Length`. The blob is addressed by its SHA-256 digest and immutable. `facet` hashes the full response, checks the expected size and digest, runs the archive verifier, and checks that the manifest's name, version, and dependencies equal the release record before extraction. The registry must not serve a different archive for the same digest.

A missing release or blob is an explicit error. A locked install does not silently choose another version, registry, or digest. Transient errors may be retried with bounded attempts and timeouts; they never trigger a new resolution.

## Resolution

A project dependency identifies its registry source and a SemVer range. Resolution fetches only version indexes and release records. It selects one version per cut name for the whole program, intersecting all incoming ranges. Stable versions are preferred; prereleases are eligible only when a requesting range explicitly includes a prerelease comparator for the same base version. Yanked versions are excluded from new resolution. Ties use canonical version text so results are deterministic.

`facet install` uses an existing lock without consulting mutable indexes. `facet update` resolves again and may change versions, but should keep an already locked version when it still satisfies all constraints unless the user requests an update for that cut. A source disagreement, incompatible ranges, missing metadata, or cycle that cannot be satisfied must report the requester paths. No artifact is downloaded until a complete graph has been selected.

## Lockfile record

A registry entry is distinct from a Git entry:

```ruby
{"greeter": {"source": "registry", "registry": "https://cuts.example", "version": "1.2.0", "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "size": 12345}}
```

The lock records every transitive cut. `source`, `registry`, `version`, `sha256`, and `size` are mandatory for registry entries. A Git entry retains `source: git`, `git`, `commit`, and optional tag-derived `version`; it never gains a registry digest by reinterpretation. Legacy Git entries without `source` remain readable. Unknown source types and mixed Git/registry fields fail validation.

A registry locked install fetches the digest-addressed blob from the locked registry, verifies the bytes, and installs it at `cuts/<name>/`. It does not run install scripts. The lockfile is written atomically after a complete successful resolution; an unsuccessful install must not leave a partly replaced cut tree.

## Publish and removal

`POST /v1/cuts/<name>/versions` accepts one verified archive and requires an
`Authorization: Bearer <token>` credential authorized for that name. The
request uses `Content-Type: application/octet-stream` and contains the exact
uncompressed tar bytes. The server validates the archive with the same contract
as `facet verify`, derives the version and dependencies from `diamond.cut`,
compares its manifest name with the path, stores the blob by digest, and
atomically creates the immutable release record. Repeating an identical request
is idempotent; different bytes for an existing tuple are rejected. A successful
first publish returns `201 Created`; an identical repeat returns `200 OK`.
Both return:

```json
{
  "protocol": 1,
  "name": "greeter",
  "version": "1.2.0",
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "size": 12345,
  "yanked": false
}
```

The optional `Idempotency-Key` header may be supplied by clients. A key is
bound to the authenticated credential and request body digest; reusing it for
another body returns `409 idempotency_conflict`. The client command is:

```text
facet publish <cut-directory> --registry <https-url> --token <token>
```

`facet` validates the cut and archive locally before sending them. It passes
the token through a temporary mode-0600 curl configuration and removes that
file after the request; it does not write credentials to the project or lock.

### `POST /v1/cuts/<name>/versions/<version>/yank`

Requires `manage:<name>` and a JSON body containing a non-empty `reason`.
Yanking is idempotent and returns the complete release record with
`"yanked": true`.

### `POST /v1/cuts/<name>/versions/<version>/unyank`

Requires `manage:<name>` and the same reason body. It restores visibility for
new resolution while preserving the archive digest and release metadata.

### `POST /v1/cuts/<name>/versions/<version>/takedown`

Requires `admin` and a non-empty reason. Takedown removes the release from read
APIs and causes locked installs to fail with `404 not_found`; it never reuses
the version tuple or silently replaces its blob. The audit event keeps the
original digest and metadata for incident review.

Yanking changes only new-resolution visibility and records who acted, when, and
why. It does not rewrite metadata or remove blob bytes. Owner changes, publish
actions, yanks, takedowns, and credential changes enter an append-only audit
log. The service must make the release row, blob reference, and corresponding
audit event durable before returning success.
