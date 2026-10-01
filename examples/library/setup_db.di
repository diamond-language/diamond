# Recreates the selected environment's database with two tables and a
# small amount of seed data -- an end-to-end smoke test for the
# SQLite3 driver, run once before app.di serves the data.

require "./lib/config/environment"

db = SQLite3.open(AppEnvironment.database_path())

# Start from scratch every run. Drop `books` first because it references
# authors; this makes the script safely re-runnable (and DESTRUCTIVE to
# whichever environment's file it is pointed at).
db.execute("DROP TABLE IF EXISTS books")
db.execute("DROP TABLE IF EXISTS authors")

# `author_id` is a plain integer, not a FOREIGN KEY constraint, which is why
# deleting an author leaves its books behind.
db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, country TEXT)")
db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER, year INTEGER, available INTEGER)")

# Insert helpers. add_author returns the new row's id so the seed books
# below can point at it.
def add_author(db, name, country)
  db.execute("INSERT INTO authors (name, country) VALUES (?, ?)", [name, country])
  db.last_insert_row_id()
end

def add_book(db, title, author_id, year, available)
  db.execute("INSERT INTO books (title, author_id, year, available) VALUES (?, ?, ?, ?)", [title, author_id, year, available])
end


# Seed data: three authors, then two books each (the second Le Guin and
# Borges books are unavailable, to make /books/available show a difference).
le_guin = add_author(db, "Ursula K. Le Guin", "USA")
calvino = add_author(db, "Italo Calvino", "Italy")
borges = add_author(db, "Jorge Luis Borges", "Argentina")

add_book(db, "The Left Hand of Darkness", le_guin, 1969, 1)
add_book(db, "The Dispossessed", le_guin, 1974, 0)
add_book(db, "Invisible Cities", calvino, 1972, 1)
add_book(db, "If on a winter's night a traveler", calvino, 1979, 1)
add_book(db, "Ficciones", borges, 1944, 1)
add_book(db, "The Aleph", borges, 1949, 0)


# Read the counts back from the database as a sanity check.
author_count = db.query("SELECT COUNT(*) AS count FROM authors")[0]["count"]
book_count = db.query("SELECT COUNT(*) AS count FROM books")[0]["count"]
puts("seeded #{author_count} authors and #{book_count} books into #{AppEnvironment.database_path()} (#{AppEnvironment.name()})")

db.close()
