# Package registry release notes — draft

Proposed release: **Diamond 0.7.0**. The final version and release date are still
pending; use 0.8.0 if another minor release ships first. This is draft announcement
text, not a published release. See [the checklist](registry-release-plan.md#release-checklist).

## Diamond's public package registry is live

Discover and install reusable Diamond libraries at
[**cuts.dilang.tech**](https://cuts.dilang.tech). The launch catalog contains
18 cuts, with versions, dependency details, archive sizes, and SHA-256 digests.
Search the catalog to find a library and copy its facet installation command.

### Try it

[Build Diamond and facet](../README.md), then run these commands in a new project
directory with both executables on your PATH:

```sh
facet init my_app
facet add logger --registry https://cuts.dilang.tech --version '^0.4.0'
facet update
diamond -e 'require_cut "logger"
Logger.new("my_app").info("Hello from the registry")'
```

Commit `diamond.cut` and `facet.lock`. On another machine, `facet install`
reproduces the locked versions and verifies downloaded archives against the
recorded sizes and digests. Use `facet update` to resolve dependencies again.
Existing Git dependencies remain supported.

### Published cuts

The initial public selection is:

- Data: `arel`, `active_record`, `database_config`, `redis`.
- HTTP and web: `http`, `gremlin`, `rack`, `cookies`, `multipart`,
  `network_safety`, `websocket`.
- GraphQL: `graphql`, `graphsql`.
- Utilities: `div`, `jobs`, `logger`, `log_viewer`.
- Registry support: `registry`.

The [reviewed inventory](registry-launch-inventory.json) records exact versions
and archive identities. Cut versions are independent of the Diamond runtime
version. Dials and the auth, discussion, karma, social, and tagging cuts are
excluded from this public selection; their source packages remain in the repo.

### Publishing and reproducibility

Publishing is available to operator-approved maintainers using scoped credentials.
[Request maintainership through GitHub issues](registry-maintainership.md).
Self-service registration is not available. Check and pack your cut with facet;
follow the [cut contract](cut-contract.md) for metadata and archive requirements.

Published versions are immutable. A yank excludes a release from new resolution
while preserving existing locks. An operator takedown removes access to that
release, including locked downloads. Publish changed contents under a new version.

### Other changes

See the [changelog](../CHANGELOG.md) for compiler diagnostics, runtime, and tooling
changes included in the final release. This draft covers the registry highlight;
review the full changelog when the release commit is selected.
