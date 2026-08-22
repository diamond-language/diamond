require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

class LibraryAuthor
  def initialize(id, name)
    @id = id
    @name = name
  end
  def id() = @id
end

class LibraryBook
  def initialize(id, title, author_id)
    @id = id
    @title = title
    @author_id = author_id
  end
  def title() = @title
  def author_id() = @author_id
end

class LibraryProfile
  def initialize(id, author_id, bio)
    @id = id
    @author_id = author_id
    @bio = bio
  end
  def bio() = @bio
end

def run_tests()
  def map_author(row)
    LibraryAuthor.new(row["id"], row["name"])
  end
  def map_book(row)
    LibraryBook.new(row["id"], row["title"], row["author_id"])
  end
  def map_profile(row)
    LibraryProfile.new(row["id"], row["author_id"], row["bio"])
  end

  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT)")
  db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER)")
  db.execute("CREATE TABLE profiles (id INTEGER PRIMARY KEY, author_id INTEGER, bio TEXT)")
  db.execute("CREATE TABLE authorships (author_id INTEGER, book_id INTEGER)")
  db.execute("INSERT INTO authors (name) VALUES (?)", ["Ada"])
  db.execute("INSERT INTO authors (name) VALUES (?)", ["Grace"])
  db.execute("INSERT INTO authors (name) VALUES (?)", ["Unpublished"])
  db.execute("INSERT INTO books (title, author_id) VALUES (?, ?)", ["Sketch", 1])
  db.execute("INSERT INTO books (title, author_id) VALUES (?, ?)", ["Notes", 1])
  db.execute("INSERT INTO books (title, author_id) VALUES (?, ?)", ["Compilers", 2])
  db.execute("INSERT INTO profiles (author_id, bio) VALUES (?, ?)", [1, "Countess of Lovelace"])
  db.execute("INSERT INTO authorships (author_id, book_id) VALUES (?, ?)", [1, 1])
  db.execute("INSERT INTO authorships (author_id, book_id) VALUES (?, ?)", [1, 2])
  db.execute("INSERT INTO authorships (author_id, book_id) VALUES (?, ?)", [2, 3])

  authors = ActiveRecord::Repository.new(Arel.table("authors"), map_author)
  books = ActiveRecord::Repository.new(Arel.table("books"), map_book)
  profiles = ActiveRecord::Repository.new(Arel.table("profiles"), map_profile)

  # HasMany#preload: every owner_id gets a key, including the one with no
  # books at all (empty Array, not a missing key).
  author_books = ActiveRecord::HasMany.new(books, "author_id")
  grouped_books = author_books.preload(db, [1, 2, 3])
  Minitest.assert_equal(2, grouped_books[1].length())
  Minitest.assert_equal("Sketch", grouped_books[1][0].title())
  Minitest.assert_equal("Notes", grouped_books[1][1].title())
  Minitest.assert_equal(1, grouped_books[2].length())
  Minitest.assert_equal("Compilers", grouped_books[2][0].title())
  Minitest.assert_equal(0, grouped_books[3].length())

  # HasOne#preload: single record or nil per owner_id.
  author_profile = ActiveRecord::HasOne.new(profiles, "author_id")
  grouped_profiles = author_profile.preload(db, [1, 2, 3])
  Minitest.assert_equal("Countess of Lovelace", grouped_profiles[1].bio())
  Minitest.assert_nil(grouped_profiles[2])
  Minitest.assert_nil(grouped_profiles[3])

  # BelongsTo#preload: keyed by the child's own foreign-key value, not by
  # the child's own id.
  book_author = ActiveRecord::BelongsTo.new(authors)
  all_books = books.all(db)
  author_ids = []
  index = 0
  while index < all_books.length()
    author_ids.push(all_books[index].author_id())
    index += 1
  end
  grouped_authors = book_author.preload(db, author_ids)
  Minitest.assert_equal(2, grouped_authors.keys().length())
  Minitest.assert_equal(true, grouped_authors.include_key?(1))
  Minitest.assert_equal(true, grouped_authors.include_key?(2))

  # HasManyThrough#preload: same shape as HasMany#preload, but through a
  # join table.
  author_books_through = ActiveRecord::HasManyThrough.new(
    books, Arel.table("authorships"), "author_id", "book_id")
  grouped_through = author_books_through.preload(db, [1, 2, 3])
  Minitest.assert_equal(2, grouped_through[1].length())
  Minitest.assert_equal(1, grouped_through[2].length())
  Minitest.assert_equal(0, grouped_through[3].length())

  db.close()
end

run_tests()
puts("active_record preload smoke ok")
