# mjolnir

A stateless data mapper on [Arel](https://github.com/diamond-language/diamond/tree/main/packages/arel). Entities are plain structs, queries are immutable values, validation is a value too, and one `Repo` object does all the I/O.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add mjolnir --registry https://cuts.dilang.tech --version "^0.2.0"
facet update
```

This installs the cut into `cuts/mjolnir/`; load it with `require_cut "mjolnir"`.

## Usage

Declare a table and the entity it maps to. The entity has no persistence methods; the builder turns a row into one, and builds come back frozen.

```ruby
require_cut "mjolnir"

struct User(id: Int, name: String, email: String, age: Int | Nil)
end

def build_user(row) = User.new(row["id"], row["name"], row["email"], row["age"])

users = Mjolnir::Schema.new("users",
  {"id": :int, "name": :string, "email": :string, "age": :int}, build_user)

db = SQLite3.open(":memory:")
db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT NOT NULL UNIQUE, age INTEGER)")
repo = Mjolnir::Repo.new(db)
```

Field types are `:int`, `:float`, `:string`, `:bool`, and `:any`.

### Changesets

A changeset whitelists and casts untrusted attributes, then validates them. It is a value: each step returns a new one, and nothing touches the database.

```ruby
def user_changeset(users, entity, attrs)
  Mjolnir::Changeset.cast(users, entity, attrs, ["name", "email", "age"])
    .validate_required(["name", "email"])
    .validate_format("email", "@")
    .unique_constraint("email")
end
```

`cast(schema, entity, attrs, permitted)` takes `nil` for a new row, or the loaded entity to change. Values are coerced to the field's type (`"36"` becomes `36`); `""` becomes `nil`; a value that cannot be coerced is an `"is invalid"` error. Only fields that differ from the entity count as changes.

Validations: `validate_required`, `validate_length(name, min, max)`, `validate_format(name, pattern)`, `validate_inclusion(name, list)`, `validate_number(name, min, max)`, plus `add_error` and `put_change`.

### Writing

`insert` and `update` return `Ok(entity)` or `Err(changeset)`. The result is sealed, so a `case` over it is checked for exhaustiveness.

```ruby
result = repo.insert(user_changeset(users, nil, {"name": "Ada", "email": "ada@example.com", "age": "36"}))

case result
when Mjolnir::Ok  then result.value().name()        # "Ada"
when Mjolnir::Err then result.error().errors()      # {"email": ["has already been taken"]}
end

ada = result.value()
repo.update(user_changeset(users, ada, {"age": 37}))   # writes only age
repo.delete(users, ada.id())                           # rows removed
```

An invalid changeset is returned as `Err` without a query. A failed constraint declared with `unique_constraint` becomes a field error; any other database error raises. Updating a row that no longer exists raises `Mjolnir::StaleEntryError`.

### Querying

```ruby
adults = users.query().where(users.column("age").gteq(18)).order_by("name").limit(10)
repo.all(adults)                         # Array of User
repo.one(users.query().where({"email": "ada@example.com"}))
repo.get(users, 1)
repo.count(adults)
```

Queries are immutable: every method returns a new one. `where` takes a Hash (ANDed equality; `nil` means `IS NULL`) or any Arel predicate from `users.column(name)`. `query.arel()` is the underlying `Arel::Query` for anything else Arel can express. A misspelled field raises `Mjolnir::UnknownFieldError` instead of generating bad SQL.

Beyond `all`/`one`/`get`/`count`:

```ruby
repo.get_by(users, {"email": "ada@example.com"})   # first row matching fields, or nil
repo.exists?(users.query().where({"age": 36}))      # Bool
users.query().where({"id": [1, 2, 3]})              # an Array is IN (an empty one matches nothing)
users.query().order_by(Arel.sql("RANDOM()"))        # a raw Arel expression orders as is
repo.update_all(users.query().where({"role": "guest"}), {"role": "user"})   # rows changed
repo.delete_all(users.query().where({"age": 0}))                             # rows removed
```

`update_all` and `delete_all` refuse a query with no condition. `insert!` and `update!` return the entity and raise `Mjolnir::InvalidChangesetError` (carrying the changeset) instead of returning `Err`.

### Associations

Declare them once, after the schemas exist, and load them explicitly. There is no lazy loading: an association is only ever fetched by `preload`, which runs one query for any number of entities.

```ruby
users.has_many("posts", posts, "user_id")
posts.belongs_to("author", users, "user_id")

listed = repo.all(users.query().limit(20))
posts_by_user = repo.preload(users, listed, "posts")
posts_by_user[listed[0].id()]            # Array of Post (empty when none)

authors = repo.preload(posts, some_posts, "author")
authors[some_posts[0].id()]              # a User, or nil
```

The result is a Hash keyed by each entity's primary key. An optional fourth argument is a `Query` on the target schema, applied before the lookup: `repo.preload(users, listed, "posts", posts.query().order_by("id", :desc))`.

### Timestamps

`Schema.new(..., "id", true)` (or `timestamps: true`) requires `created_at` and `updated_at` integer fields. `insert` stamps both with the current epoch seconds, and `update` and `update_all` refresh `updated_at`, unless the changeset sets them itself.

### Transactions

```ruby
repo.transaction() do |tx|
  tx.insert(user_changeset(users, nil, attrs))
end
```

The block's value is returned. A returned `Err` or a raised exception rolls back; anything else commits.

## Notes

The default visitor targets SQLite; pass `Mjolnir::Repo.new(db, Arel::PostgreSQLVisitor.new())` for another dialect. Mjolnir keeps no identity map and no change tracking, so each `Repo` call is exactly one round trip.
