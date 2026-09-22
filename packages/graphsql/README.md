# graphsql

Select GraphQL-requested columns and preload associations through `active_record`.

## Installation

Install the cut at `cuts/graphsql/` and load it with `require_cut "graphsql"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

Requires `graphql`, `active_record`.

## Usage

```diamond
require_cut "graphsql"

author_mapping = GraphSQL::Mapping.new(Author.repository(), "Author")
author_mapping.column("name").column("country", "homeCountry")
author_mapping.association("books", "books", book_mapping)
results = GraphSQL.resolve(Author.all(), db, context["lookahead"], author_mapping)
```

## Notes

Pass a relation, database connection, GraphQL lookahead, and mapping to `GraphSQL.resolve`. A nested association may return an Array; a flat query returns a relation.
