require "../../packages/diamond-active_record/lib/diamond-active_record"
require "../../lib/minitest"

class LibraryAuthor
  def initialize(id, name)
    @id = id
    @name = name
  end
  def id() = @id
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
  def map_profile(row)
    LibraryProfile.new(row["id"], row["author_id"], row["bio"])
  end

  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT)")
  db.execute("CREATE TABLE profiles (id INTEGER PRIMARY KEY, author_id INTEGER, bio TEXT)")
  db.execute("INSERT INTO authors (name) VALUES (?)", ["Ada"])
  db.execute("INSERT INTO authors (name) VALUES (?)", ["Grace"])
  db.execute("INSERT INTO profiles (author_id, bio) VALUES (?, ?)", [1, "Countess of Lovelace"])

  authors = ActiveRecord::Repository.new(Arel.table("authors"), map_author)
  profiles = ActiveRecord::Repository.new(Arel.table("profiles"), map_profile)
  author_profile = ActiveRecord::HasOne.new(profiles, "author_id")

  ada = authors.find(db, 1)
  profile = author_profile.get(db, ada.id())
  Minitest.assert_equal("Countess of Lovelace", profile.bio())

  grace = authors.find(db, 2)
  Minitest.assert_nil(author_profile.get(db, grace.id()))

  db.close()
end

run_tests()
puts("diamond_active_record has_one smoke ok")
