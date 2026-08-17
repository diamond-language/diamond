db = SQLite3.open(":memory:")
db.execute("CREATE TABLE items (id INTEGER PRIMARY KEY, label TEXT, qty INTEGER, price REAL, note TEXT)")
db.execute("INSERT INTO items (label, qty, price, note) VALUES (?, ?, ?, ?)",
  ["widget", 3, 2.5, nil])
db.query("SELECT label, qty, price, note FROM items")
