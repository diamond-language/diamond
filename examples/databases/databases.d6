# Configure either PostgreSQL or MySQL with a DatabaseConfig JSON file.
require "../../packages/database_config/lib/database_config"

# Exercises one live connection: a parameterized insert in a committed
# transaction, then an update in a transaction that is rolled back.
def demonstrate(db, adapter)
  # MySQL/MariaDB temporary tables default to an engine that ignores
  # transactions, which would make the ROLLBACK below silently do nothing.
  # InnoDB is transactional. PostgreSQL needs no suffix.
  suffix = adapter == "mysql" || adapter == "mariadb" ? " ENGINE=InnoDB" : ""

  # TEMPORARY: private to this connection and dropped on close, so the
  # example leaves nothing behind in a real database.
  db.execute("CREATE TEMPORARY TABLE diamond_example_people (id INTEGER PRIMARY KEY, name VARCHAR(100))#{suffix}")

  # Commit path. The `?` placeholders are bound by the driver, never pasted
  # into the SQL, which is why the apostrophe in "Ada O'Brien" is safe.
  db.execute("BEGIN")
  begin
    db.execute("INSERT INTO diamond_example_people (id, name) VALUES (?, ?)", [1, "Ada O'Brien"])
    db.execute("COMMIT")
  rescue error: StandardError
    # Never leave the transaction open on failure; re-raise the original.
    db.execute("ROLLBACK")
    raise error
  end

  # Rollback path: change the row, then abandon the change.
  db.execute("BEGIN")
  db.execute("UPDATE diamond_example_people SET name = ? WHERE id = ?", ["temporary", 1])
  db.execute("ROLLBACK")

  # Verify both behaviors at once: the row exists (the insert committed)
  # and still holds the ORIGINAL name (the update was rolled back).
  rows = db.query("SELECT name FROM diamond_example_people WHERE id = ?", [1])
  if rows.length() != 1 || rows[0]["name"] != "Ada O'Brien"
    raise "parameter binding or transaction rollback failed"
  end
  puts("PASS: #{adapter}: parameterized insert/query, commit, and rollback")
end

# The example is opt-in: with no config it SKIPs (exit 0) so test runs on a
# machine without a database server still pass.
def main()
  path = ENV["DIAMOND_DATABASE_EXAMPLE_CONFIG"]
  if path == nil
    puts("SKIP: set DIAMOND_DATABASE_EXAMPLE_CONFIG to a PostgreSQL or MySQL JSON config file")
    return 0
  end

  # Load the JSON config, then check the adapter before connecting. The
  # SQL above is written for PostgreSQL and MySQL/MariaDB only.
  config = DatabaseConfig.load(path)
  adapter = DatabaseConfig.adapter(config)
  unless ["postgres", "postgresql", "mysql", "mariadb"].include?(adapter)
    raise ArgumentError.new("this example requires PostgreSQL or MySQL")
  end

  # Always close the connection, even if demonstrate raises.
  db = DatabaseConfig.open(config)
  begin
    demonstrate(db, adapter)
  ensure
    db.close()
  end

  0
end

exit(main())
