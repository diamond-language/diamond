db = SQLite3.open(":memory:")
db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, x INTEGER)")
db.execute("INSERT INTO t (x) VALUES (1)")
db.execute("INSERT INTO t (x) VALUES (2)")
db.execute("INSERT INTO t (x) VALUES (3)")
updated = db.execute("UPDATE t SET x = x + 1 WHERE x >= 2")
deleted = db.execute("DELETE FROM t WHERE x = 1")
[updated, deleted]
