# Configure either PostgreSQL or MySQL with a DatabaseConfig JSON file.
require "../../packages/database_config/lib/database_config"

def demonstrate(db, adapter)
  suffix = adapter == "mysql" || adapter == "mariadb" ? " ENGINE=InnoDB" : ""
  db.execute("CREATE TEMPORARY TABLE diamond_example_people (id INTEGER PRIMARY KEY, name VARCHAR(100))#{suffix}")
  db.execute("BEGIN")
  begin
    db.execute("INSERT INTO diamond_example_people (id, name) VALUES (?, ?)", [1, "Ada O'Brien"])
    db.execute("COMMIT")
  rescue error: StandardError
    db.execute("ROLLBACK")
    raise error
  end

  db.execute("BEGIN")
  db.execute("UPDATE diamond_example_people SET name = ? WHERE id = ?", ["temporary", 1])
  db.execute("ROLLBACK")
  rows = db.query("SELECT name FROM diamond_example_people WHERE id = ?", [1])
  if rows.length() != 1 || rows[0]["name"] != "Ada O'Brien"
    raise "parameter binding or transaction rollback failed"
  end
  puts("PASS: #{adapter}: parameterized insert/query, commit, and rollback")
end

def main()
  path = ENV["DIAMOND_DATABASE_EXAMPLE_CONFIG"]
  if path == nil
    puts("SKIP: set DIAMOND_DATABASE_EXAMPLE_CONFIG to a PostgreSQL or MySQL JSON config file")
    return 0
  end
  config = DatabaseConfig.load(path)
  adapter = DatabaseConfig.adapter(config)
  unless ["postgres", "postgresql", "mysql", "mariadb"].include?(adapter)
    raise ArgumentError.new("this example requires PostgreSQL or MySQL")
  end
  db = DatabaseConfig.open(config)
  begin
    demonstrate(db, adapter)
  ensure
    db.close()
  end
  0
end

exit(main())
