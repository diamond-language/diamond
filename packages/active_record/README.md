# active_record

Repositories, associations, validation, migrations, and an optional model layer for SQL-backed Diamond applications.

## Installation

Install the cut at `cuts/active_record/` and load it with `require_cut "active_record"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

Requires `arel`.

## Design

Nothing here inspects your schema or conjures methods from symbols. Column lists, associations, validators and row mappers are all passed in explicitly, as ordinary values and functions. There are three layers, each built on the one before:

1. **`Repository`** — persistence primitives over one table (`find`, `create`, `update`, `delete`, …).
2. **`Relation`** — a lazy, chainable query built by `Repository#relation`.
3. **`Model`** — an optional Rails-flavoured base class over the two above.

All database arguments (`db`) are an open `SQLite3` (or PostgreSQL/MariaDB) connection, or an `ActiveRecord::InstrumentedConnection` wrapping one.

## Repository

```diamond
require_cut "active_record"

def map_author(row) = row

repository = ActiveRecord::Repository.new(
  Arel.table("authors"),
  map_author,
  "id"
)
repository.find(db, 1)
repository.create(db, {"name": "Ada", "country": "UK"})
repository.update(db, 1, {"country": "England"})
repository.delete(db, 1)
```

`Repository.new(table, mapper, id_column = "id", visitor = nil, validator = nil, before_save = nil, after_save = nil, lock_column = nil, column_names = [], inheritance_column = nil, association_reflections = [])`

| Argument | Meaning |
|---|---|
| `table` | An `Arel.table(...)`. |
| `mapper` | `(row Hash) -> object`. Called on every row a query returns. |
| `id_column` | Primary key column. |
| `visitor` | Arel dialect. `nil` means SQLite; pass another visitor for PostgreSQL/MariaDB. |
| `validator` | `(attributes, exclude_id) -> Array[String]`. See [Validators](#validators). |
| `before_save` | `(db, attributes, on) -> attributes`. `on` is `:create`, `:update` or `:destroy`. Its return value is what is written (ignored for `:destroy`). |
| `after_save` | `(db, attributes, on)`. Return value ignored. |
| `lock_column` | Turns on optimistic locking. See [Optimistic locking](#optimistic-locking). |
| `column_names` | Array of column names. Required for `has_column?`, which GraphSQL uses to validate its column mappings. |
| `inheritance_column` | Single-table-inheritance column, if any. |
| `association_reflections` | Array of `AssociationReflection`. See [Associations](#associations). |

Methods: `all(db)`, `find(db, id)` (returns `nil` when absent), `where(db, conditions_hash)`, `relation()`, `create(db, attributes)`, `update(db, id, attributes, expected_lock_version = nil)`, `delete(db, id)`, `find_in_batches(db, callback, batch_size = 1000)`, `find_each(db, callback, batch_size = 1000)`.

`find_in_batches`/`find_each` page by primary key (`WHERE id > last_id ORDER BY id LIMIT n`), not by `OFFSET`, so they stay fast and don't skip or repeat rows if the table changes while you iterate.

## Relation

`Model.all()`, `Model.where(...)` and `repository.relation()` return a lazy `Relation`. Nothing touches the database until a terminal call.

```diamond
relation = Game.where({"owner_id": 7})
  .order(Arel.table("games").column("id").asc())
  .skip(20)
  .limit(10)
rows  = relation.to_a(db)    # Array of mapped objects
first = relation.first(db)   # one object or nil
total = relation.count(db)
```

Chain methods (each returns a new `Relation`): `where(hash_or_predicate)`, `order(column_or_columns)`, `take(n)` / `limit(n)`, `skip(n)` / `offset(n)`, `select(columns)` / `reselect(columns)`, `includes(association_names)`.

`first` adds **no implicit `ORDER BY`**. Without a preceding `order`, which row you get is up to the database.

`includes("player")` (or an Array of names) eager-loads those associations in one batched query per association, via the repository's registered reflections. An unknown name raises `AssociationNotFoundError`.

## Model

A model is a class that subclasses `ActiveRecord::Model` and fills in a handful of explicit hooks. Diamond has no class-body macros, so there is a small amount of per-model boilerplate. It is the same shape for every model:

```diamond
class Game < ActiveRecord::Model
  attr_accessor owner_id: Int, title: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @owner_id = attributes["owner_id"]
    @title = attributes["title"]
  end

  # The inverse of the repository's mapper: current field values as a Hash.
  def to_attributes() = {"owner_id": @owner_id, "title": @title}

  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  # Association readers are written by hand.
  def owner(db) = self.belongs_to(Account.repository()).get(db, @owner_id)
  def leaderboards(db) = self.has_many(Leaderboard.repository(), "game_id").all(db, self.id())
end

def build_game(row) = Game.new(row)   # the repository's mapper
```

Both `repository()` and `self.repository()` are required, and each model must have its own `@@repository`. A class variable is scoped to the class whose code reads it, so an inherited accessor would give every model the same shared slot.

Wire the repository up once at startup:

```diamond
Game.configure(ActiveRecord::Repository.new(
  Arel.table("games"), build_game, "id", nil, game_validator,
  nil, nil, nil, ["id", "owner_id", "title"]))
```

Instance methods: `id()`, `persisted?()`, `save(db)`, `destroy(db)`, `save!`/`destroy!` (identical; the plain forms already raise on failure), `read_attribute(name)`, `as_json()` / `to_json()`.

Class methods: `find(db, id)`, `all()`, `where(conditions)`, `find_by(db, conditions)`, `create(db, attributes)`, `create!`, `find_each`, `find_in_batches`. `all` and `where` return a `Relation` (no `db` argument).

`save` inserts when the model has no id and updates when it has one. After an insert it sets the model's id from `last_insert_row_id()`.

### Secure passwords

```diamond
class Account < ActiveRecord::Model
  attr_accessor email: String, password_digest   # untyped on purpose, see below
  # ...
end

account.secure_password=("correct horse")   # stores a bcrypt digest, cost 12
account.authenticate("correct horse")       # -> true / false
```

`password_digest` is deliberately **not** type-annotated. A typed accessor raises on a `nil` read, and `authenticate` relies on getting `nil` back for an account with no password set. For a different bcrypt cost, call `BCrypt.hash(password, cost)` and assign the digest yourself.

## Associations

Associations are registered per repository as `AssociationReflection` values. Nothing is inferred from names.

```diamond
ActiveRecord::AssociationReflection.new(
  "leaderboards",           # name
  "has_many",               # "belongs_to" | "has_one" | "has_many"
  Leaderboard.repository(), # target repository (an object, not a name)
  "game_id",                # foreign key
  "id")                     # owner key (default "id")
```

Pass them as the last argument of `Repository.new`. Because a reflection holds the target repository *object*, two models that reference each other (`Account` has one `Player`, `Player` belongs to an `Account`) can't both be built in one step. Build each repository first without reflections, then rebuild it with them once its targets exist.

`reflection.preload(db, records, scope = nil)` loads the association for a whole batch of records in one query and stores the result on each record. The model reads it back with `association_loaded?(name)` and `preloaded_association(name)`; `set_preloaded_association(name, value)` fills the cache by hand. A common reader shape is:

```diamond
def player(db)
  if self.association_loaded?("player")
    self.preloaded_association("player")
  else
    self.has_one(Player.repository(), "account_id").get(db, self.id())
  end
end
```

## Validators

A validator is a function `(attributes, exclude_id) -> Array[String]`. An empty array means valid. `Repository#create` and `#update` run it before any SQL and raise `ActiveRecord::ValidationError` (message list on `error.errors()`) on failure. `exclude_id` is the record's own id on update and `nil` on create.

`ActiveRecord::Validators` builds them:

| Builder | Checks |
|---|---|
| `presence(field, message = nil)` | Not `nil` and not `""`. |
| `length(field, minimum = nil, maximum = nil, message = nil)` | `value.length()`. A `nil` value passes (that is `presence`'s job). |
| `numericality(field, message = nil)` | `Int` or `Float`. `nil` fails. |
| `format(field, pattern, message = nil)` | `pattern.match?(value)`. `nil` fails. |
| `inclusion(field, values, message = nil)` | `values.include?(value)`. |
| `uniqueness(db, table, field, visitor = nil, message = nil, id_column = "id")` | Queries the database. On update it excludes the record's own row, so saving a record unchanged doesn't conflict with itself. |
| `combine([validators])` | Concatenates every validator's errors. |

```diamond
validator = ActiveRecord::Validators.combine([
  ActiveRecord::Validators.presence("handle"),
  ActiveRecord::Validators.length("handle", 3, 30),
  ActiveRecord::Validators.format("handle", Regexp.new("^[A-Za-z0-9_]+$")),
  ActiveRecord::Validators.uniqueness(db, Arel.table("players"), "handle")
])
```

`uniqueness` closes over `db`, so build it per connection. When connections are per-worker or per-request, build the validator where the connection is created.

## Transactions

```diamond
ActiveRecord::Transaction.run(db) do
  game.save(db)
  leaderboard.save(db)     # if this raises, the game insert is rolled back too
end
```

`run` issues `BEGIN`, then `COMMIT` on a normal return, or `ROLLBACK` and re-raises on any `StandardError`. It returns the block's value. SQL databases can't nest `BEGIN`, so inside an open transaction use `Transaction.run_nested(db, "savepoint_name", callback)`, which uses a `SAVEPOINT` and rolls back only to it. Savepoint names must be letters, digits and underscores, and can't start with a digit.

## Migrations

A migration is a **Hash**, not a class: a stable unique `"version"` string, an `"up"` function and an optional `"down"` function.

```diamond
def create_games_up(db)
  db.execute("CREATE TABLE games (id INTEGER PRIMARY KEY, title TEXT NOT NULL)")
end
def create_games_down(db)
  db.execute("DROP TABLE games")
end
def create_games_migration() = {
  "version": "20261005120000", "up": create_games_up, "down": create_games_down
}

migrations = [create_games_migration()]
ActiveRecord::Migrator.run(db, migrations)
```

`Migrator` methods, all taking the same `migrations` Array: `run(db, migrations)`, `rollback(db, migrations, steps = 1)`, `pending(db, migrations)`, `applied(db, migrations)`.

- Applied versions are recorded in a `schema_migrations` table, created on first use.
- The Array's own order is the only order. Versions are never sorted.
- `run` is safe to call on every boot. Each pending migration runs in its own transaction, so a failure part-way leaves earlier migrations committed.
- `rollback` reverses the most recently applied migrations, found by walking the Array backwards. A migration with no `"down"` raises rather than being skipped.
- Duplicate versions in the Array raise `ArgumentError`.

DDL is plain SQL. Arel renders only `SELECT`/`INSERT`/`UPDATE`/`DELETE`.

## Optimistic locking

Give the repository a `lock_column` (an integer column) and `update` requires `expected_lock_version`. It matches on that value, bumps it by one in the same statement, and raises `StaleObjectError` if nothing matched, meaning someone else updated or deleted the row first. `Model#save` passes the loaded version automatically.

## Dirty tracking

`DirtyAttributes` wraps a plain Hash row and reports what changed, for use with `Repository#update`:

```diamond
dirty = ActiveRecord::DirtyAttributes.new(row)
dirty["country"] = "England"
repository.update(db, row["id"], dirty.changes()) if dirty.changed?()
```

Also: `changed_keys()`, `attribute_changed?(key)`, `to_h()`.

## Query logging

`ActiveRecord::InstrumentedConnection.new(connection, subscriber)` wraps a connection and calls `subscriber(event_hash)` around every `query`/`execute`. Events carry `phase` (`started`/`completed`/`failed`), `query_id`, `operation`, `sql`, `bind_count`, and on completion `duration_ms` and `rows`/`affected`. Bind values are never included.
