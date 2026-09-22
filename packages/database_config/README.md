# database_config

Load JSON database configuration and open a native SQLite, PostgreSQL, or MySQL-compatible connection.

## Installation

Install the cut at `cuts/database_config/` and load it with `require_cut "database_config"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

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

```ruby
require_cut "database_config"

config = DatabaseConfig.load("config/database.json", ENV["DIAMOND_ENV"])
db = DatabaseConfig.open(config)
```

## Notes

The root JSON value can also be a single connection object. Supported adapters: `sqlite`/`sqlite3`, `postgres`/`postgresql`, and `mysql`/`mariadb`. Use `password_env` to read a password from an environment variable.
