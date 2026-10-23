db = SQLite3.open(":memory:")
db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, x INTEGER)")
db.execute("INSERT INTO t (x) VALUES (10)")
first_id = db.last_insert_row_id()
db.execute("INSERT INTO t (x) VALUES (20)")
second_id = db.last_insert_row_id()
[first_id, second_id]
