# Cut registry protocol, version 1

Status: proposed wire contract. No hosted registry or registry install exists yet. This specification defines the metadata and artifact boundary that `facet` will use; it does not change Git dependency behavior.

## Identity

A release is identified by `(registry URL, cut name, version)`. The registry URL is a normalized HTTPS origin plus an optional base path, with no query or fragment. `facet` stores that exact configured URL in the lockfile and never substitutes another registry. A name follows the publishable cut name rule in [cut-contract.md](cut-contract.md). A version is canonical SemVer 2.0.0 without a leading `v` or build metadata. Each tuple is immutable, including its metadata and archive digest.

Protocol responses include `"protocol": 1`. Unknown protocol versions fail closed. Clients ignore only explicitly optional display fields; unknown fields in identity, dependency, or artifact records fail. Responses use UTF-8 JSON with bounded sizes. JSON object keys must be unique. Registry metadata is data: neither resolution nor installation executes code from a cut.

## Read API

All paths below are relative to the configured registry base. Path segments are percent-encoded from validated ASCII names and versions. Responses must use `Content-Type: application/json` and may use `ETag` and `Cache-Control`; clients may cache them, but a lockfile install does not query the version index.

### `GET /v1/cuts/<name>/versions`

Returns an array of available version records, sorted by SemVer precedence and then by canonical version text. Each record includes `version`, `yanked`, and the release metadata URL. The index can omit yanked releases for new resolution, but a direct release lookup must still work for a locked release unless an exceptional takedown occurred. The client treats the order as presentation only and sorts deterministically itself.

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

`POST /v1/cuts/<name>/versions` accepts one verified archive and requires a credential authorized for that name. The server validates the archive with the same contract as `facet verify`, compares its manifest name and version with the request, stores the blob by digest, and atomically creates the immutable release record. Repeating an identical request is idempotent; different bytes for an existing tuple are rejected.

Yanking changes only new-resolution visibility and records who acted, when, and why. It does not rewrite metadata or remove blob bytes. An exceptional takedown has its own audited operator path; locked installs then fail with an explicit unavailable-release error. Owner changes, publish actions, yanks, and takedowns enter an append-only audit log. Credential format, owner policy, and service operations require a separate deployment design before public launch.
