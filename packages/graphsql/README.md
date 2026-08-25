# graphsql

GraphQL-lookahead-driven column selection and association preloading for
Diamond's `graphql` and `active_record` packages, ported from GraphSQL 0.5.1.

Mappings are explicit runtime values because Diamond GraphQL types are runtime
`GraphQL::ObjectType` values rather than Ruby subclasses:

```diamond
author_mapping = GraphSQL::Mapping.new(Author.repository(), "Author")
author_mapping.column("name").column("country", "homeCountry")
author_mapping.association("books", "books", book_mapping)
```

An optional parent mapping supplies inherited columns and associations; local
entries override the same GraphQL field name.

Pass the model relation, its database connection, the resolver's
`context["lookahead"]`, and its mapping to `GraphSQL.resolve`:

```diamond
result = GraphSQL.resolve(
  Author.all(), db, context["lookahead"], author_mapping)
```

The resolver selects only the primary key plus requested mapped columns,
belongs-to foreign keys, a configured STI inheritance column, and explicitly
required columns. Requested associations without a target mapping use
`Relation#includes`. Target-mapped nested associations use scoped batch
preloads recursively, retaining the child foreign key needed to attach
`has_many`/`has_one` rows. The flat path stays lazy and returns a Relation; a
nested optimized path must load records for scoped preloading and returns an
Array.

`required_associations` and `required_columns` are optional trailing Arrays:

```diamond
GraphSQL.resolve(relation, db, lookahead, mapping,
  ["organization"], ["computed_source"])
```

Unknown mapped or required columns raise `GraphSQL::UnknownColumnError` before
SQL execution. Requesting two mapped GraphQL aliases for the same nested
association raises `GraphSQL::AliasedAssociationError`, avoiding two
incompatible column-limited scopes sharing one model association cache.
