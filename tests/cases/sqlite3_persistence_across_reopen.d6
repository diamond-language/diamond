path = "/tmp/diamond_sqlite3_persistence_test.db"

first = SQLite3.open(path)
first.execute("DROP TABLE IF EXISTS people")
first.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT)")
first.execute("INSERT INTO people (name) VALUES (?)", ["Ada"])
first.close()

second = SQLite3.open(path)
rows = second.query("SELECT name FROM people")
second.close()
rows
