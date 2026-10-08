# graphsql

Select GraphQL-requested columns and preload associations through `active_record`.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add graphsql --registry https://cuts.dilang.tech --version "^0.1.3"
facet update
```

This installs the cut into `cuts/graphsql/`; load it with `require_cut "graphsql"`. `active_record` and `graphql` are installed with it.

## Usage

GraphSQL sits between a [`graphql`](https://github.com/diamond-language/diamond/tree/main/packages/graphql) resolver and an [`active_record`](https://github.com/diamond-language/diamond/tree/main/packages/active_record)
relation. Describe how GraphQL fields map to columns and associations once, then hand each
resolver's lookahead to `GraphSQL.resolve`. This complete program wires an in-memory SQLite
database, two models, a schema, and a query:

```ruby
# app.di
require_cut "graphql"
require_cut "graphsql"

class Book < ActiveRecord::Model
  attr_accessor author_id: Int, title: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @author_id = attributes["author_id"]
    @title = attributes["title"]
  end

  def to_attributes() = {"author_id": @author_id, "title": @title}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
end

class Author < ActiveRecord::Model
  attr_accessor name: String, country: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @name = attributes["name"]
    @country = attributes["country"]
  end

  def to_attributes() = {"name": @name, "country": @country}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  def books(db) = self.has_many(Book.repository(), "author_id").all(db, self.id())
end

def build_book(row) = Book.new(row)
def build_author(row) = Author.new(row)

db = SQLite3.open(":memory:")
db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, country TEXT)")
db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, author_id INTEGER, title TEXT)")

Book.configure(ActiveRecord::Repository.new(Arel.table("books"), build_book, "id", nil, nil, nil, nil, nil,
  ["id", "author_id", "title"]))
Author.configure(ActiveRecord::Repository.new(Arel.table("authors"), build_author, "id", nil, nil, nil, nil, nil,
  ["id", "name", "country"], nil, [
    ActiveRecord::AssociationReflection.new("books", "has_many", Book.repository(), "author_id", "id")]))

Author.create(db, {"name": "Ada", "country": "UK"})
Book.create(db, {"author_id": 1, "title": "Notes"})
Book.create(db, {"author_id": 1, "title": "Sketch"})

book_mapping = GraphSQL::Mapping.new(Book.repository(), "Book")
book_mapping.column("title")
author_mapping = GraphSQL::Mapping.new(Author.repository(), "Author")
author_mapping.column("id").column("name").column("country", "homeCountry")
author_mapping.association("books", "books", book_mapping)

module AuthorResolvers
  module_function
  def id(object, args, context) = "#{object.id()}"
  def name(object, args, context) = object.name()
  def home_country(object, args, context) = object.country()
  def books(object, args, context) = object.preloaded_association("books")
end
module BookResolvers
  module_function
  def title(object, args, context) = object.title()
end
module QueryResolvers
  module_function
  def authors(object, args, context)
    planned = GraphSQL.resolve(Author.all(), context["db"], context["lookahead"], context["mapping"])
    if planned is ActiveRecord::Relation then planned.to_a(context["db"]) else planned end
  end
end

book_type = GraphQL::ObjectType.new("Book")
book_type.field("title", GraphQL::ScalarType.string().non_null(), BookResolvers.title)
author_type = GraphQL::ObjectType.new("Author")
author_type.field("id", GraphQL::ScalarType.id().non_null(), AuthorResolvers.id)
author_type.field("name", GraphQL::ScalarType.string(), AuthorResolvers.name)
author_type.field("homeCountry", GraphQL::ScalarType.string(), AuthorResolvers.home_country)
author_type.field("books", GraphQL::ListType.of(book_type), AuthorResolvers.books)
query_type = GraphQL::ObjectType.new("Query")
query_type.field("authors", GraphQL::ListType.of(author_type), QueryResolvers.authors)
schema = GraphQL::Schema.new()
schema.query(query_type)

result = schema.execute("{ authors { name homeCountry books { title } } }", {}, {"db": db, "mapping": author_mapping})
puts(JSON.stringify(result))
# {"data":{"authors":[{"name":"Ada","homeCountry":"UK","books":[{"title":"Notes"},{"title":"Sketch"}]}]}}
```

The `authors` query selects only `id`, `name`, and `country` from `authors` (the columns the query
named, plus the primary key), then loads all the books in a single `books` query rather than one
per author. In a server, build the mapping and schema once per worker and pass the database
connection in the `execute` context, as shown in the `graphql` README.

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
- Lookahead over-approximates on polymorphic fields (see the [graphql README](https://github.com/diamond-language/diamond/blob/main/packages/graphql/README.md#lookahead)), so GraphSQL may select a column or preload an association a runtime type never uses. It never selects too little.
