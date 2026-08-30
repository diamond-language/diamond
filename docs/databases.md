# Databases

[I/O and native services](io.md) · Previous: [Networking and signals](networking.md) · Next: [Time](time.md)

## SQLite3: `SQLite3.open`/`.execute`/`.query`/`.prepare`/`.last_insert_row_id`/`.close`

```ruby
db = SQLite3.open("data.db")
db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")
db.execute("INSERT INTO people (name, age) VALUES (?, ?)", ["Ada", 30])
db.execute("INSERT INTO people (name, age) VALUES (:name, :age)", {"name": "Grace", "age": 40})
id = db.last_insert_row_id()
db.query("SELECT * FROM people WHERE age >= ?", [18])
# => [{id: 1, name: Ada, age: 30}, {id: 2, name: Grace, age: 40}]

insert = db.prepare("INSERT INTO people (name, age) VALUES (?, ?)")
insert.execute(["Linus", 50])
insert.execute(["Ester", 60])
insert.close()

readonly = SQLite3.open("data.db", "r")
readonly.query("SELECT * FROM people")
readonly.close()

db.close()
```

`SQLite3.open(path)` opens (creating if missing, sqlite3's own default)
via `sqlite3_open`, the system `libsqlite3` — a genuinely external C
dependency, unlike `Regexp`'s in-repo `reginold`, linked as a plain
`-lsqlite3` rather than a bundled static archive. `SQLite3.open(path,
mode)` opens via `sqlite3_open_v2` instead, with `mode` a `String`: `"r"`
(read-only, error if missing), `"rw"` (read-write, error if missing), or
`"rwc"` (read-write, created if missing — spelling out `sqlite3_open`'s
own default explicitly). An unrecognized or non-`String` `mode` raises
`TypeError` (a Diamond-level call-shape mistake, checked at runtime since
`mode` is an arbitrary expression, not a compile-time literal) — this is
the same "string mode, runtime-validated" shape `File.open`'s own `mode`
argument already uses, not a new mechanism. A failed open raises a
rescuable `SQLite3Error` — a dedicated exception class, not `IOError`,
since a corrupt or unopenable database file is a sqlite-specific
condition and every other failure this type can raise (a bad statement,
a bind/step error) is `SQLite3Error` too, giving callers one class to
`rescue` against for anything this type raises.

A `SQLite3` value is a new GC-managed heap object kind
(`DIAMOND_OBJECT_SQLITE3`), a thin wrapper around a `sqlite3 *` — the
same shape as `File`'s `DiamondFileHandle` around a `FILE *`, including
the same closed/open sentinel (`db` nulled by `#close()`, checked before
any other operation) and the same "sweeping an unreached-but-still-open
handle closes it as a safety net" GC behavior. `#close()` closes via
`sqlite3_close_v2`, not plain `sqlite3_close`: a `Statement` prepared
from this connection (`#prepare`, below) is an independent GC object
that can outlive the connection's own Diamond-level reachability, and
plain `sqlite3_close` fails (leaving the real OS-level connection open
and leaked, since this call site never checked its return code) with
any unfinalized statement still outstanding. `_v2` never fails: it
closes immediately if nothing is outstanding, or defers the actual close
until every associated `Statement` is itself finalized — sqlite3's own
documented idiom for exactly this ownership shape, and the reason a
`Statement` can keep being used (see below) after its owning connection
is already Diamond-level `#close()`d.

`SQLite3.open` is recognized in the compiler the same way `File.open`
is, compiling to a single `DIAMOND_OP_SQLITE3_OPEN dest, path, mode`
instruction (`mode` a compile-time `NIL` when omitted from the call, the
sentinel the VM reads as "use plain `sqlite3_open`, unchanged"; the same
compile-time-default-when-omitted shape `exit()`'s optional argument
already uses). `.execute`/`.query`/`.prepare`/`.last_insert_row_id`/
`.close` are native `DIAMOND_OP_INVOKE` dispatch on a
`DIAMOND_OBJECT_SQLITE3` receiver, the same mechanism `File`'s own
methods use:

- `.execute(sql)` / `.execute(sql, params)` prepares and runs one
  statement, discarding any rows it produces, and returns the number of
  rows it changed (`sqlite3_changes`) as an `Int` — the useful return
  value for `INSERT`/`UPDATE`/`DELETE`/DDL.
- `.query(sql)` / `.query(sql, params)` prepares and runs one statement,
  collecting every row into `Array[Hash]` (column name → typed value).
- `.prepare(sql)` compiles `sql` once and returns a reusable `Statement`
  (below), for running the same statement many times without repaying
  `sqlite3_prepare_v2`'s parse/plan cost on every call — see "Statement"
  below.
- `.last_insert_row_id()` returns the `Int` rowid of the most recent
  successful `INSERT` on this connection (`sqlite3_last_insert_rowid`).
- `.close()` is idempotent, exactly like `File#close`.

`params`, when given to `.execute`/`.query`, is either an `Array` bound
*positionally* to a statement's `?` placeholders (1-indexed, sqlite3's
own convention), or a `Hash` bound *by name* to `:name` placeholders
(`sqlite3_bind_parameter_index`, looked up with the `:` sigil prepended
to each Diamond `String` key — only the `:name` spelling is recognized
at the Diamond level, not `@name`/`$name`, matching the bare-name-key
convention most host-language sqlite3 drivers already use). Either form
is the injection-safe way to include a value in a query; never
interpolate a value directly into the SQL string. In both forms, a
parameter-count mismatch raises `ArgumentError`; for named binds, a Hash
key with no matching `:name` placeholder in the statement also raises
`ArgumentError`, and a non-`String` Hash key raises `TypeError`. An
unsupported Diamond value anywhere in `params` (anything but
`Int`/`Float`/`String`/`Bool`/`Nil`) raises `TypeError`. Anything other
than `Array`/`Hash`/omitted for `params` itself is `TypeError`. Bind/
column type mapping:

| Diamond → sqlite3 (`params`) | sqlite3 → Diamond (`query` results) |
|---|---|
| `Int` → `sqlite3_bind_int64` | `INTEGER` → `Int` |
| `Float` → `sqlite3_bind_double` | `FLOAT` → `Float` |
| `String` → `sqlite3_bind_text` | `TEXT`/`BLOB` → `String` (a Diamond `String` is already a raw byte buffer, so a blob's raw bytes need no separate representation) |
| `Bool` → bound as `Int` 0/1 | `NULL` → `Nil` |
| `Nil` → `sqlite3_bind_null` | |

`sqlite3_prepare_v2` only compiles the first statement up to a `;` and
leaves the rest unexecuted — silently dropping a second statement
chained after the first would be a real correctness trap, so
`.execute`/`.query`/`.prepare` reject anything left over besides
trailing whitespace with a clear `SQLite3Error` rather than ignoring it.
`.execute`/`.query` each do their own prepare→bind→step→finalize.

### `Statement`: `SQLite3#prepare`'s `.execute`/`.query`/`.close`

`db.prepare(sql)` compiles `sql` once (`sqlite3_prepare_v2`, the same
single-statement guard as `.execute`/`.query`) and returns a `Statement`
— another new GC-managed heap object kind
(`DIAMOND_OBJECT_SQLITE3_STATEMENT`), a thin wrapper around a
`sqlite3_stmt *`, with the same closed/open sentinel and GC-sweep-closes-
as-safety-net shape as `SQLite3` itself (`#close()`/GC sweep/VM teardown
all `sqlite3_finalize`, idempotently).

- `.execute(params = nil)` / `.query(params = nil)` — same argument
  shape, return values, and bind semantics as the connection's own
  `.execute`/`.query` above (positional `Array`, named `Hash`, or
  omitted), except bound against the *already-prepared* statement rather
  than compiling it fresh: `sqlite3_reset` + `sqlite3_clear_bindings`
  rewind and clear any previous call's bind state first (so an omitted
  bind on this call can't accidentally reuse a stale value bound on a
  previous one), then the new `params` are bound and the statement is
  stepped — never finalized, so it stays ready for the next call.
- `.close()` is idempotent, exactly like `SQLite3#close`.

A `Statement` holds no back-reference to its owning `SQLite3` connection
value (`sqlite3_db_handle(stmt)` recovers the real `sqlite3*` for error
messages when needed) — its lifetime is independent, per the `#close()`
`_v2` note above: preparing a statement, letting the `SQLite3` connection
value itself go out of Diamond-level scope (or explicitly `#close()`ing
it) while the `Statement` is still reachable and in use, is well-defined
and worked out via `sqlite3_close_v2`'s deferred-close semantics, not
merely untested.

## PostgreSQL: `PostgreSQL.open`/`.execute`/`.query`/`.last_insert_row_id`/`.close`

```ruby
db = PostgreSQL.open("host=localhost dbname=myapp user=myuser password=secret")
db.execute("CREATE TABLE people (id SERIAL PRIMARY KEY, name TEXT, age INT)")
db.execute("INSERT INTO people (name, age) VALUES (?, ?)", ["Ada", 30])
id = db.last_insert_row_id()
db.query("SELECT * FROM people WHERE age >= ?", [18])
# => [{id: 1, name: Ada, age: 30}]
db.close()
```

`PostgreSQL.open(conninfo)` connects via `PQconnectdb`, the system `libpq`
(via `-lpq`) — a genuinely external C dependency exactly like `SQLite3`'s
`libsqlite3`. `conninfo` is passed through untouched to libpq itself, so
either its keyword/value form (`"host=... port=... dbname=... user=...
password=..."`) or a `postgresql://user:pass@host:port/dbname` URI works,
whatever libpq's own parser accepts. A failed connection raises a
rescuable `PostgreSQLError` — a dedicated exception class, not `IOError`,
for the same reason `SQLite3Error` exists: every failure this type can
raise (a bad conninfo, a malformed statement, a bind/exec error) is
`PostgreSQLError`, giving callers one class to `rescue` against.

A `PostgreSQL` value is a new GC-managed heap object kind
(`DIAMOND_OBJECT_POSTGRES`), a thin wrapper around a `PGconn *` — the
same shape as `SQLite3`'s `DiamondSqlite3Handle` around a `sqlite3 *`,
including the same closed/open sentinel (`conn` nulled by `#close()`,
checked before any other operation) and the same "sweeping an
unreached-but-still-open handle closes it as a safety net" GC behavior.

`PostgreSQL.open` is recognized in the compiler the same way `SQLite3.open`
is, compiling to a single `DIAMOND_OP_POSTGRES_OPEN dest, conninfo`
instruction. `.execute`/`.query`/`.last_insert_row_id`/`.close` are native
`DIAMOND_OP_INVOKE` dispatch on a `DIAMOND_OBJECT_POSTGRES` receiver, the
same mechanism `SQLite3`'s own methods use:

- `.execute(sql)` / `.execute(sql, params)` runs one statement via
  `PQexecParams` and returns the number of rows it affected (`PQcmdTuples`,
  parsed as an `Int`; `""` — e.g. from `CREATE TABLE` — means `0`) as an
  `Int`, the useful return value for `INSERT`/`UPDATE`/`DELETE`/DDL.
- `.query(sql)` / `.query(sql, params)` runs one statement and collects
  every row into `Array[Hash]` (column name → typed value), same shape
  `SQLite3#query` produces.
- `.last_insert_row_id()` runs `SELECT lastval()` and returns its `Int`
  result — Postgres has no direct equivalent of `sqlite3_last_insert_
  rowid`, so this approximates it for a `serial`/`GENERATED ALWAYS AS
  IDENTITY` column. It raises `PostgreSQLError` (`lastval` itself failing
  with "lastval is not yet defined in this session") if no sequence has
  been used yet on this connection — the more idiomatic Postgres pattern
  for a specific insert's id is `INSERT ... RETURNING id` via `.query()`
  directly, not this method.
- `.close()` is idempotent, exactly like `SQLite3#close`.

`params`, when given, is an `Array` bound *positionally* — but unlike
`SQLite3`, which binds directly to sqlite3's own native `?` placeholders,
Postgres's C API (`PQexecParams`) requires numbered `$1`/`$2`/...
placeholders. `SQLite3`'s `?` spelling is kept at the Diamond level anyway,
for API consistency between the two drivers and so either is a drop-in
target for the same `#query(sql, params)` contract (see
[`packages/arel/README.md`](../packages/arel/README.md)): the driver
translates `?` to `$1`/`$2`/... internally before calling `PQexecParams`,
skipping any `?` inside a single-quoted string literal (`''` is the
standard SQL escaped quote). A `?` used outside a string literal always
counts as a placeholder — Postgres's own JSONB "key exists" `?` operator,
for instance, isn't distinguishable from one here and isn't usable through
the params-array call form. A parameter-count mismatch raises
`ArgumentError`; an unsupported Diamond value in `params` (anything but
`Int`/`Float`/`String`/`Bool`/`Nil` — notably including a bignum-promoted
`Int`, matching `SQLite3`'s own same restriction) raises `TypeError`. Bind/
column type mapping:

| Diamond → Postgres (`params`, text format) | Postgres → Diamond (`query` results, by OID) |
|---|---|
| `Int` → decimal text | `int2`/`int4`/`int8` → `Int` |
| `Float` → `%.17g`, or `Infinity`/`-Infinity`/`NaN` | `float4`/`float8`/`numeric` → `Float` (lossy for a `numeric` outside `Float` precision) |
| `String` → passed through unchanged | `text`/`varchar`/`bpchar` → `String` |
| `Bool` → `"true"`/`"false"` | `bool` → `Bool` |
| `Nil` → SQL `NULL` (a null `paramValues` entry) | `NULL` → `Nil` |
| | any other type (`date`/`timestamp`/`json`/`jsonb`/`uuid`/`bytea`/arrays/...) → the raw `String` libpq's text format already returns |

The last row is a deliberate, documented scope cut, not silent data loss:
`bytea` in particular stays Postgres's default hex-text spelling
(`\x...`), not decoded to raw bytes. `PQexecParams` itself refuses more
than one SQL command per call regardless of parameter count, so unlike
`SQLite3` (which needs an explicit tail-content check after
`sqlite3_prepare_v2`), no separate multi-statement guard is needed here —
Postgres's own rejection surfaces as an ordinary `PostgreSQLError`.

Out of scope for this driver, deliberately: an Arel dialect visitor for
Postgres (a separate project once there's a real second dialect to
validate Arel's grammar seams against — see
[`packages/arel/ROADMAP.md`](../packages/arel/ROADMAP.md)), prepared/named
statements, asynchronous/non-blocking connections, connection pooling, and
binary-format result decoding.

## MySQL: `MySQL.open`/`.execute`/`.query`/`.last_insert_row_id`/`.close`

```ruby
db = MySQL.open("localhost", "myuser", "secret", "myapp", 3306)
db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY AUTO_INCREMENT, name TEXT, age INT)")
db.execute("INSERT INTO people (name, age) VALUES (?, ?)", ["Ada", 30])
id = db.last_insert_row_id()
db.query("SELECT * FROM people WHERE age >= ?", [18])
# => [{id: 1, name: Ada, age: 30}]
db.close()
```

`MySQL.open(host, user, password, database, port)` connects via
`mysql_real_connect`, MariaDB Connector/C (via `-lmariadb`, libmysqlclient-API-
compatible) — a genuinely external C dependency exactly like `SQLite3`'s
`libsqlite3` and `PostgreSQL`'s `libpq`. Unlike `PostgreSQL.open`'s single
conninfo `String` (libpq parses that key=value format itself), these are five
required, explicit positional arguments — MariaDB Connector/C's
`mysql_real_connect` takes discrete fields with no such conninfo string to
parse, so this driver takes them the same explicit way rather than inventing
a DSN mini-language it would then own the parsing/escaping/documentation of.
`port` has no default; a bind port default would hide a real, easy-to-get-
wrong choice (`3306` vs. a nonstandard port) rather than a rarely-needed
knob. A failed connection raises a rescuable `MySQLError` — a dedicated
exception class for the same reason `SQLite3Error`/`PostgreSQLError` exist:
every failure this type can raise (unreachable host, bad credentials, a
malformed statement, a bind/exec error) is `MySQLError`, giving callers one
class to `rescue` against.

A `MySQL` value is a new GC-managed heap object kind
(`DIAMOND_OBJECT_MYSQL`), a thin wrapper around a `MYSQL *` — the same shape
as `SQLite3`/`PostgreSQL`'s own handles, including the same closed/open
sentinel (`conn` nulled by `#close()`, checked before any other operation)
and the same "sweeping an unreached-but-still-open handle closes it as a
safety net" GC behavior.

`MySQL.open` is recognized in the compiler the same way `SQLite3.open`/
`PostgreSQL.open` are, compiling to a single
`DIAMOND_OP_MYSQL_OPEN dest, host, user, password, database, port`
instruction. `.execute`/`.query`/`.last_insert_row_id`/`.close` are native
`DIAMOND_OP_INVOKE` dispatch on a `DIAMOND_OBJECT_MYSQL` receiver, the same
mechanism `SQLite3`/`PostgreSQL`'s own methods use:

- `.execute(sql)` / `.execute(sql, params)` prepares and executes one
  statement (via `mysql_stmt_*`, MariaDB Connector/C's real out-of-band
  binary parameter binding — not string interpolation/escaping) and returns
  `mysql_stmt_affected_rows` as an `Int`, the useful return value for
  `INSERT`/`UPDATE`/`DELETE`/DDL.
- `.query(sql)` / `.query(sql, params)` runs one statement and collects
  every row into `Array[Hash]` (column name → typed value), same shape
  `SQLite3#query`/`PostgreSQL#query` produce.
- `.last_insert_row_id()` is a direct `mysql_insert_id(conn)` call —
  simpler than `PostgreSQL#last_insert_row_id`'s own `SELECT lastval()`
  round-trip, since MySQL's client library tracks the connection's last
  `AUTO_INCREMENT` value itself. Unlike `lastval()`, it has no "not yet
  defined this session" failure mode: an unused connection just reads
  back `0`.
- `.close()` is idempotent, exactly like `SQLite3#close`/`PostgreSQL#close`.

`params`, when given, is an `Array` bound *positionally* through real
prepared-statement parameter binding — MySQL's own placeholder spelling is
already `?`, the same as `SQLite3`'s, so (unlike `PostgreSQL`) no
translation step is needed. A parameter-count mismatch (checked against
`mysql_stmt_param_count` after preparing) raises `ArgumentError`; an
unsupported Diamond value in `params` (anything but
`Int`/`Float`/`String`/`Bool`/`Nil`, matching both other drivers' same
restriction) raises `TypeError`. Bind/column type mapping:

| Diamond → MySQL (`params`, prepared-statement binary protocol) | MySQL → Diamond (`query` results, by field type) |
|---|---|
| `Int` → `MYSQL_TYPE_LONGLONG` | `TINY`/`SHORT`/`LONG`/`LONGLONG`/`INT24`/`YEAR` → `Int` |
| `Float` → `MYSQL_TYPE_DOUBLE` | `FLOAT`/`DOUBLE`/`DECIMAL`/`NEWDECIMAL` → `Float` |
| `String` → `MYSQL_TYPE_STRING` | `STRING`/`VAR_STRING`/`BLOB`/anything else → the raw `String` its text form decodes to |
| `Bool` → `MYSQL_TYPE_TINY` (`0`/`1`) | (no native boolean type — see below) |
| `Nil` → `MYSQL_TYPE_NULL` | `NULL` → `Nil` |

The last result row is a deliberate, documented scope cut, not silent data
loss: dates/times/JSON/bit fields all stay MySQL's own text spelling. MySQL
has no native boolean type — `TINYINT(1)` is only a convention, indistinguishable
at the protocol level from any other `TINYINT` column — so every integer
type decodes as `Int` here, the same tradeoff `SQLite3` (also boolean-less)
already makes, unlike `PostgreSQL`'s real `bool` OID. This connection is
never opened with `CLIENT_MULTI_STATEMENTS`, so unlike `SQLite3` (which
needs an explicit tail-content check) or `PostgreSQL` (whose `PQexecParams`
refuses multiple commands itself), a semicolon-separated second statement
here is simply a syntax error `mysql_stmt_prepare` itself raises as an
ordinary `MySQLError`.

An Arel dialect visitor for MySQL now exists --
`Arel::MySQLVisitor` (`packages/arel/lib/arel.di`), verified against a
live MySQL 8 server (`packages/arel/README.md`,
`packages/arel/ROADMAP.md`) -- reusing this driver unchanged, since it
was never MariaDB-specific at the native layer. Still out of scope for
this driver, deliberately, for the same reasons `PostgreSQL`'s own scope
cuts are: connection pooling and `unix_socket`/`CLIENT_MULTI_STATEMENTS`
connection options.
