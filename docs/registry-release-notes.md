# Diamond 0.7.0

Release date: 2026-09-23.

## Diamond's public package registry is live

Discover and install reusable Diamond libraries at
[**cuts.dilang.tech**](https://cuts.dilang.tech). The launch catalog contains
18 cuts, with versions, dependency details, archive sizes, and SHA-256 digests.
Search the catalog to find a library and copy its facet installation command.

### Try it

[Build Diamond and facet](https://github.com/diamond-language/diamond/blob/v0.7.0/README.md), then run these commands in a new project
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

The [reviewed inventory](https://github.com/diamond-language/diamond/blob/v0.7.0/docs/registry-launch-inventory.json) records exact versions
and archive identities. Cut versions are independent of the Diamond runtime
version. Dials and the auth, discussion, karma, social, and tagging cuts are
excluded from this public selection; their source packages remain in the repo.

### Publishing and reproducibility

Publishing is available to operator-approved maintainers using scoped credentials.
[Request maintainership through GitHub issues](https://github.com/diamond-language/diamond/blob/main/docs/registry-maintainership.md).
Self-service registration is not available. Check and pack your cut with facet;
follow the [cut contract](https://github.com/diamond-language/diamond/blob/v0.7.0/docs/cut-contract.md) for metadata and archive requirements.

Published versions are immutable. A yank excludes a release from new resolution
while preserving existing locks. An operator takedown removes access to that
release, including locked downloads. Publish changed contents under a new version.

### Other changes

- Compiler diagnostics now preserve their message text and show more precise
  type information, including missing members in non-exhaustive `case` errors.
- The x86-64 JIT covers more typed string and collection operations, string
  slicing, type checks, and method chains through known or narrowed receivers.
- Persistent AOT caches include runtime source and build fingerprints, preventing
  incompatible cached runtime archives from being reused across checkouts.
- The interactive REPL adds a `clear` command.

See the [full changelog](https://github.com/diamond-language/diamond/blob/v0.7.0/CHANGELOG.md#070--2026-09-23)
for implementation details and limitations. Diamond remains pre-1.0.
