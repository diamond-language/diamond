# PostgreSQL and MySQL transactions

This example uses the existing `database_config` package to connect to either
PostgreSQL or MySQL/MariaDB. It binds values with `?` parameters, commits an
insert, rolls back an update, and checks that the original name (including an
apostrophe) survives. The MySQL temporary table explicitly uses InnoDB for
transaction support.

From the repository root, copy and edit either `postgresql.json` or `mysql.json`
from this directory, then run:

```sh
DIAMOND_DATABASE_EXAMPLE_CONFIG=/absolute/path/to/config.json \
  build/diamond examples/databases/databases.di
```

Use an existing test database and a user allowed to create temporary tables.
Only a connection-local temporary table is created; closing the connection
removes it. No persistent tables are created or changed.

For PostgreSQL the sample uses libpq connection information: use a password
file or `PGPASSWORD` if authentication requires it. For MySQL, export
`DIAMOND_EXAMPLE_DB_PASSWORD` (an empty value is allowed). Keep credentials out
of committed files. See [database configuration](../../packages/database_config/README.md)
for configuration fields and [native drivers](../../docs/databases.md) for APIs.

If `DIAMOND_DATABASE_EXAMPLE_CONFIG` is unset, the program prints `SKIP` and exits
successfully. If it is set, even to an empty value, configuration, connection,
SQL, or verification errors fail the run. An unavailable configured server is
never treated as a skip.

```sh
bash examples/databases/smoke_test.sh
```

The smoke test checks skipping and invalid configurations in both interpreted
and compiled modes. Set the same config variable to additionally run both modes
against your server. Run it once per adapter to verify both live drivers.
