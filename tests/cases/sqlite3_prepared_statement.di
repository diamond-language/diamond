db = SQLite3.open(":memory:")
db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")

insert = db.prepare("INSERT INTO people (name, age) VALUES (?, ?)")
insert.execute(["Ada", 30])
insert.execute(["Grace", 40])
insert.execute(["Linus", 50])
insert.close()

select_stmt = db.prepare("SELECT * FROM people WHERE age >= ? ORDER BY age")
first = select_stmt.query([35])
second = select_stmt.query([45])
select_stmt.close()

no_bind_stmt = db.prepare("SELECT COUNT(*) AS c FROM people")
count = no_bind_stmt.query()
changed = no_bind_stmt.query()  # #query is idempotent to call again -- resets internally
no_bind_stmt.close()

closed_query_failed = false
begin
  select_stmt.query([0])
rescue error: SQLite3Error
  closed_query_failed = true
end
select_stmt.close()  # idempotent

[first, second, count, changed, closed_query_failed]
