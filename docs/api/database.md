# Databases

[API reference](index.md) · Previous: [Networking](network.md)

Three drivers share one shape: `execute` runs a statement that changes data
and returns the number of rows changed, and `query` returns the rows as an
Array of Hashes from column name to value. Pass values as `params`, never by
interpolating them into the SQL. A placeholder is `?` in all three drivers;
SQLite also accepts `:name` with a Hash. Bound values must be `Int`, `Float`,
`String`, `Bool` or `nil`; anything else raises `TypeError`, and the wrong
number of values raises `ArgumentError`. [Databases](../databases.md) has the
full type mappings.

```ruby
db = SQLite3.open(":memory:")
db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")
db.execute("INSERT INTO people (name, age) VALUES (?, ?)", ["Ada", 36])   # => 1
db.execute("INSERT INTO people (name, age) VALUES (:name, :age)", {"name": "Grace", "age": 45})
db.last_insert_row_id()                                                     # => 2
db.query("SELECT name, age FROM people WHERE age > ?", [40])   # => [{"name": "Grace", "age": 45}]
db.close()
```

## SQLite3

Errors raise `SQLite3Error`. Opening a file needs the `filesystem` sandbox
capability.

### `SQLite3.open(path, mode = nil) -> SQLite3`

Opens the database at `path`, creating it if missing; `":memory:"` is a
private in-memory database. `mode` is `"r"` (read-only), `"rw"` (must exist)
or `"rwc"` (the default behaviour).

### `execute(sql, params = nil) -> Int`

Runs one statement and returns the number of rows it changed. A statement
that returns rows, other than a `PRAGMA`, raises `TypeError`; use `query`.
More than one statement in `sql` raises `SQLite3Error`.

### `query(sql, params = nil) -> Array`

Runs one statement and returns its rows.

### `prepare(sql) -> Statement`

Compiles `sql` once for repeated use.

### `last_insert_row_id() -> Int`

The row ID of the latest successful `INSERT` on this connection.

### `close()`

Closes the connection. Closing again does nothing.

## Statement

A statement prepared by `SQLite3#prepare`. It stays usable after its
connection is closed, until it is closed itself.

### `execute(params = nil) -> Int`

### `query(params = nil) -> Array`

Run the statement with fresh `params`, like the connection's methods.

### `close()`

Releases the statement.

```ruby
db = SQLite3.open(":memory:")
db.execute("CREATE TABLE t (n INTEGER)")
insert = db.prepare("INSERT INTO t (n) VALUES (?)")
[1, 2, 3].each() do |n|
  insert.execute([n])
end
insert.close()
db.query("SELECT SUM(n) AS total FROM t")   # => [{"total": 6}]
```

## PostgreSQL

Errors raise `PostgreSQLError`.

<!-- doc-check: skip -->
```ruby
db = PostgreSQL.open("postgresql://app:secret@localhost/app")
rows = db.query("INSERT INTO people (name) VALUES (?) RETURNING id", ["Ada"])
rows[0]["id"]
```

### `PostgreSQL.open(conninfo) -> PostgreSQL`

Connects with a libpq connection string: `"host=... dbname=... user=..."` or
a `postgresql://` URI.

### `execute(sql, params = nil) -> Int`

### `query(sql, params = nil) -> Array`

As for SQLite3. Integer, float, numeric, text and boolean columns become d6
values; other types come back as their text form.

### `last_insert_row_id() -> Int`

The value of `lastval()`, the most recent sequence value on this connection.
Raises `PostgreSQLError` before any sequence has been used. Prefer
`INSERT ... RETURNING id`.

### `close()`

Closes the connection.

## MySQL

Works with MySQL and MariaDB. Errors raise `MySQLError`.

<!-- doc-check: skip -->
```ruby
db = MySQL.open("localhost", "app", "secret", "app", 3306)
db.execute("UPDATE people SET age = age + 1 WHERE id = ?", [1])
```

### `MySQL.open(host, user, password, database, port) -> MySQL`

Connects; every argument is required.

### `execute(sql, params = nil) -> Int`

### `query(sql, params = nil) -> Array`

As for SQLite3. Integer and decimal columns become d6 values; dates, JSON and
other types come back as text.

### `last_insert_row_id() -> Int`

The `AUTO_INCREMENT` value from the latest insert, or `0`.

### `close()`

Closes the connection.
