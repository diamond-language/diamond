# database_config

Load JSON database configuration and open a native SQLite, PostgreSQL, or MySQL-compatible connection.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add database_config --registry https://cuts.dilang.tech --version "^0.1.1"
facet update
```

This installs the cut into `cuts/database_config/`; load it with `require_cut "database_config"`.

## Configuration

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

## Usage

Keep connection settings in a JSON file, one entry per environment, and pick the entry
at startup:

```json
{
  "development": {"adapter": "sqlite", "database": "app.db"},
  "production": {"adapter": "postgres", "host": "db.internal", "database": "app",
                 "user": "app", "password_env": "APP_DB_PASSWORD"}
}
```

```ruby
require_cut "database_config"

config = DatabaseConfig.load("config/database.json", ENV["DIAMOND_ENV"] || "development")
db = DatabaseConfig.open(config)    # an open SQLite3/PostgreSQL/MariaDB connection
```

`db` is the connection every [`active_record`](https://github.com/diamond-language/diamond/tree/main/packages/active_record) repository and `arel`
query takes; see that README's Quick start for a complete program. For a PostgreSQL or
MySQL entry, pass the matching Arel visitor to your repositories (`PostgreSQLVisitor`,
`MySQLVisitor`, or `MariaDBVisitor`).

## Notes

`DatabaseConfig.load(path, name = nil)` validates the entry and raises `ArgumentError`
for a missing or malformed one; without `name` the file's root object is the connection.
Supported adapters: `sqlite`/`sqlite3`, `postgres`/`postgresql`, and `mysql`/`mariadb`.
Use `password_env` to read the password from an environment variable instead of
committing it. A PostgreSQL entry may instead give a complete `"connection"` string.
