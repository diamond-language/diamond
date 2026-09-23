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

Writes use `File.publish`: synchronize a private temporary file, atomically
link it to the digest path without replacement, remove the temporary name,
and synchronize the containing directory. The root must be an existing,
trusted directory on a local filesystem supporting hard links and directory
synchronization. Blob files are private to the service account (mode 0600).

A crash before publication can leave `.diamond-publish-*` staging files;
these are ignored by digest lookup and may be removed when writers are stopped.
A failure after linking can leave a complete published file even though the
call raises. Recovery must verify that file before recording a release or
retrying. The database publish transaction is still pending.

Run `make test-registry-package` from the repository root. The tests verify
expected exception types, unchanged bytes after a duplicate write, invalid
digest rejection, and corrupt-file detection.
