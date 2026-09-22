# arel roadmap

Arel builds immutable SQL statements and renders them with dialect visitors. See [README.md](README.md) for usage and [VISITORS.md](VISITORS.md) for the visitor contract.

## Next work

- Add SQL expressions when an application needs them and the AST can represent them without raw SQL.
- Keep bind order and identifier quoting correct for every supported dialect.
- Test dialect differences against the corresponding database server.

Connection management, row mapping, and change tracking belong to higher-level libraries.
