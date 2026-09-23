# registry

Storage primitives for the Diamond cut registry.

This first slice provides the SQLite schema and a content-addressed blob store.
The HTTP service, authentication flow, archive validation, and publish
transaction will build on these primitives.

Install with `facet` and load with `require_cut "registry"`.

`BlobStore.read` verifies the stored bytes against their digest and raises
`IOError` for corrupt or incomplete files. `contains?` returns false when the
file is missing, unreadable, or fails verification. A duplicate `put` raises
`IOError` without replacing the existing file.

The current writer uses exclusive creation directly at the digest path. It
does not yet provide atomic visibility or crash durability: an interrupted
write can leave an incomplete file that readers reject and operators must
remove before retrying. Staged publication with filesystem synchronization is
a prerequisite for the publish transaction and hosted service.

Run `make test-registry-package` from the repository root. The tests verify
expected exception types, unchanged bytes after a duplicate write, invalid
digest rejection, and corrupt-file detection.
