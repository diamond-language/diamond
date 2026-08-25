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
