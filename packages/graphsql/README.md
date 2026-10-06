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

## What it does

A GraphQL query names exactly the fields it wants. Without help, a resolver loads whole rows and then one association query per parent row. GraphSQL reads the field's lookahead (`context["lookahead"]`, set by the `graphql` executor) and rewrites an `ActiveRecord::Relation` so that:

- it `SELECT`s only the columns the query asked for, plus the primary key and any foreign key an association needs;
- every requested association is preloaded in **one batched query per association**, including nested ones, instead of one per parent row.

## Mappings

A `Mapping` pairs one GraphQL object type with one repository, and says which GraphQL fields are columns and which are associations. Diamond types are runtime values rather than class declarations, so this is a separate fluent value.

```diamond
player = GraphSQL::Mapping.new(Player.repository(), "Player")
player.column("id").column("handle")

score = GraphSQL::Mapping.new(Score.repository(), "Score")
score.column("id").column("value")
score.association("player", "player", player)

leaderboard = GraphSQL::Mapping.new(Leaderboard.repository(), "Leaderboard")
leaderboard.column("id").column("name").column("higher_is_better", "higherIsBetter")
leaderboard.association("scores", "scores", score)
```

- `Mapping.new(repository, type_name, parent = nil)`
- `column(column_name, field_name = nil)` maps a GraphQL field to a column. `field_name` defaults to the column name; give it when the GraphQL name differs (`column("higher_is_better", "higherIsBetter")`).
- `association(name, field_name = nil, target_mapping = nil)` maps a GraphQL field to a reflection registered on the repository. `name` is the reflection name. `target_mapping` is the `Mapping` for the associated type, and nested selections need it.
- `parent` lets a mapping inherit the columns and associations of another.

Both methods return the mapping so calls chain.

Fields you do **not** map are left alone. If a GraphQL field is computed by a resolver rather than read from a column, don't map it; make sure the columns it depends on are listed in `required_columns` (below).

Every mapped column must exist in the repository's `column_names`, so construct the repository with that list. A mapping to a missing column raises `GraphSQL::UnknownColumnError` rather than producing broken SQL.

## Resolving

```diamond
GraphSQL.resolve(relation, db, lookahead, mapping, required_associations = [], required_columns = [])
```

- `relation` is an `ActiveRecord::Relation` over the mapping's repository, such as `Game.where(...)` or `Game.all().order(...).limit(20)`. Filtering, ordering and pagination are yours; GraphSQL only adjusts what is selected and preloaded.
- `required_columns` forces extra columns into the `SELECT` that a custom resolver needs even when the query didn't ask for them.
- `required_associations` forces extra associations to be preloaded for the same reason.

**The return type depends on the query shape.**

- If nothing nested has to be planned, you get back an `ActiveRecord::Relation` (flat, or with `includes` applied). Finish it with `.to_a(db)` or `.first(db)`.
- If the query selects nested associations, you get back an `Array` of already-loaded records.

Callers therefore check which they received:

```diamond
planned = GraphSQL.resolve(relation, db, context["lookahead"], mapping)
rows = if planned is ActiveRecord::Relation then planned.to_a(db) else planned end
```

If the relation's repository is not the mapping's repository, or a non-relation is passed, it is returned unchanged.

## Reading preloaded data in resolvers

Preloaded associations are stored on each record. Field resolvers should prefer them and fall back to a query only when the record wasn't planned through GraphSQL:

```diamond
def scores(leaderboard, args, context)
  if leaderboard.association_loaded?("scores")
    leaderboard.preloaded_association("scores")
  else
    leaderboard.scores(context["db"])
  end
end
```

## Limitations and errors

- Selecting the same association under two different GraphQL field names in one query raises `GraphSQL::AliasedAssociationError`, because both would map to a single preload.
- Polymorphic and `has_many :through` associations are preloaded through the plain `includes` path instead of nested column selection.
- Lookahead over-approximates on polymorphic fields (see the [graphql README](../graphql/README.md#lookahead)), so GraphSQL may select a column or preload an association a runtime type never uses. It never selects too little.
