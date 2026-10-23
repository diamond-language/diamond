db = SQLite3.open(":memory:")
db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")

db.execute("INSERT INTO people (name, age) VALUES (:name, :age)", {"name": "Ada", "age": 30})
db.execute("INSERT INTO people (name, age) VALUES (:name, :age)", {"name": "Grace", "age": 40})
by_name = db.query("SELECT * FROM people WHERE name = :name", {"name": "Ada"})

stmt = db.prepare("SELECT * FROM people WHERE age >= :min AND age <= :max")
ranged = stmt.query({"min": 25, "max": 35})
stmt.close()

unknown_key_rejected = false
begin
  db.execute("INSERT INTO people (name, age) VALUES (:name, :age)", {"name": "X", "ag": 1})
rescue error: ArgumentError
  unknown_key_rejected = true
end

count_mismatch_rejected = false
begin
  db.execute("INSERT INTO people (name, age) VALUES (:name, :age)", {"name": "X"})
rescue error: ArgumentError
  count_mismatch_rejected = true
end

non_string_key_rejected = false
begin
  db.execute("INSERT INTO people (name, age) VALUES (:name, :age)", {1: "X", 2: 1})
rescue error: TypeError
  non_string_key_rejected = true
end

[by_name, ranged, unknown_key_rejected, count_mismatch_rejected, non_string_key_rejected]
