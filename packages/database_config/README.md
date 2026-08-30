# database_config

JSON-backed connection configuration for Diamond's native SQLite3,
PostgreSQL, and MySQL-compatible drivers.

```json
{
  "development": {"adapter": "sqlite3", "database": "app.db"},
  "production": {
    "adapter": "postgresql",
    "host": "db.internal",
    "port": 5432,
    "database": "app",
    "user": "app",
    "password_env": "DATABASE_PASSWORD"
  }
}
```

```ruby
require_cut "database_config"

config = DatabaseConfig.load("config/database.json", ENV["DIAMOND_ENV"])
db = DatabaseConfig.open(config)
```

The root may also be one connection object, in which case omit the second
argument to `load`. Supported adapters are `sqlite`/`sqlite3`,
`postgres`/`postgresql`, and `mysql`/`mariadb`. MariaDB uses Diamond's
MySQL-compatible native driver.

Passwords can be written as `"password"`, but `"password_env"` is preferred
for deployed applications. PostgreSQL may alternatively provide a complete
libpq `"connection"` string. Defaults are `127.0.0.1`, port 5432 for
PostgreSQL, port 3306 for MySQL-compatible servers, and SQLite mode `rwc`.
PostgreSQL field values may contain whitespace; if a value contains a quote or
backslash, supply a complete libpq `connection` string so libpq owns parsing.

`load` validates the selected entry before returning it. Invalid JSON,
missing entries, unsupported adapters, wrong field types, and unset password
environment variables raise clear, rescuable errors. `open` returns the
native database handle, so the ordinary API in
[the database guide](../../docs/databases.md) applies unchanged.

Run the self-contained test with:

```sh
make test-database-config-package
```
