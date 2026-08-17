# Creates library.db (in this same directory) with two tables and a
# small amount of seed data -- an end-to-end smoke test for the
# SQLite3 driver, run once before app.di serves the data.

def db_path()
  "library.db"
end

db = SQLite3.open(db_path())

db.execute("DROP TABLE IF EXISTS books")
db.execute("DROP TABLE IF EXISTS authors")

db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, country TEXT)")
db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER, year INTEGER, available INTEGER)")

def add_author(db, name, country)
  db.execute("INSERT INTO authors (name, country) VALUES (?, ?)", [name, country])
  db.last_insert_row_id()
end

def add_book(db, title, author_id, year, available)
  db.execute("INSERT INTO books (title, author_id, year, available) VALUES (?, ?, ?, ?)", [title, author_id, year, available])
end

le_guin = add_author(db, "Ursula K. Le Guin", "USA")
calvino = add_author(db, "Italo Calvino", "Italy")
borges = add_author(db, "Jorge Luis Borges", "Argentina")

add_book(db, "The Left Hand of Darkness", le_guin, 1969, 1)
add_book(db, "The Dispossessed", le_guin, 1974, 0)
add_book(db, "Invisible Cities", calvino, 1972, 1)
add_book(db, "If on a winter's night a traveler", calvino, 1979, 1)
add_book(db, "Ficciones", borges, 1944, 1)
add_book(db, "The Aleph", borges, 1949, 0)

author_count = db.query("SELECT COUNT(*) AS count FROM authors")[0]["count"]
book_count = db.query("SELECT COUNT(*) AS count FROM books")[0]["count"]
puts("seeded #{author_count} authors and #{book_count} books into #{db_path()}")

db.close()
