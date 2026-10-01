# models: record classes whose accessors are generated at run time from a
# JSON schema -- a tour of Diamond's metaprogramming.
#
#   diamond models.di testdata/schema.json
require "./lib/collection"

# The classes are declared here; their fields come from the schema.
class Book < Model
end

class Author < Model
end

# Parses a JSON file; `ensure` closes it even if parsing fails.
def read_json(path: String)
  file = File.open(path, "r")
  begin
    JSON.parse(file.read())
  ensure
    file.close()
  end
end

# Runs a block and prints either its result or the error it raised, so the
# demo can show failures without stopping.
def attempt(label: String, &action)
  begin
    puts("  #{label}: #{yield()}")
  rescue error: TypeError | FrozenError | NoMethodError
    puts("  #{label}: #{error.class()}: #{error.message()}")
  end
end

def main(args: Array[String]) -> Int
  if args.length() != 1
    warn("usage: models SCHEMA.json")
    return 64
  end

  # Generate each class's methods from its section of the schema. `readonly`
  # then swaps isbn's writer for a write-once one.
  schema = read_json(args[0])
  Book.fields(schema["Book"])
  Author.fields(schema["Author"])
  Book.readonly("isbn")

  # Show that the methods exist on Book, and on Book ONLY: Author has `born`
  # but Book does not, and `name` is Author's alone.
  puts("== generated methods")
  probe = Book.new()
  ["title", "title=", "in_print?", "name", "born"].each() do |name|
    puts("  Book responds to #{name}: #{probe.respond_to?(to_sym(name))}")
  end
  puts("  Author responds to born: #{Author.new().respond_to?(:born)}")

  puts("")
  # Build records through `build`, so every value is type-checked.
  puts("== building records")
  books = Collection.new([
    Book.build({"title": "Dune", "pages": 412, "isbn": "978-0441013593", "in_print": true}),
    Book.build({"title": "The Left Hand of Darkness", "pages": 304, "isbn": "978-0441478125", "in_print": true}),
    Book.build({"title": "Babel-17", "pages": 173, "isbn": "978-0375706691", "in_print": false}),
  ])
  books.each() do |book| puts("  #{book}") end
  dune = books.find_by_title("Dune")
  puts("  Dune in print? #{dune.in_print?()}")

  puts("")
  # Three edits: pages changes for real; the title is changed and then put
  # back, so it must NOT appear in `changes`.
  puts("== change tracking")
  dune.pages = 896
  dune.title = "Dune (illustrated)"
  dune.title = "Dune"
  puts("  changes: #{dune.changes()}")

  puts("")
  # Failures: a wrong type, a write to a read-only field, and a record
  # that breaks the schema's max and min rules.
  puts("== type checks, read-only fields, validation")
  attempt("set pages to a String") do dune.pages = "many" end
  attempt("change the isbn") do dune.isbn = "000" end
  draft = Book.build({"title": "An unreasonably long working title for a novel", "pages": 0})
  puts("  draft valid? #{draft.valid?()}")
  draft.errors().each() do |problem| puts("    #{problem}") end

  puts("")
  # method_missing in action: find_by_/where_ work for any field; anything
  # else (count_by_title) is still a NoMethodError.
  puts("== dynamic finders")
  puts("  find_by_pages(173): #{books.find_by_pages(173)}")
  puts("  where_in_print(true): #{books.where_in_print(true).map() do |book| book.title() end}")
  puts("  find_by_title(\"Nope\"): #{books.find_by_title("Nope")}")
  attempt("books.count_by_title(\"Dune\")") do books.count_by_title("Dune") end
  author = Author.build({"name": "Ursula K. Le Guin", "born": 1929})
  puts("  #{author}; public_send(\"born\") = #{author.public_send("born")}")
  0
end

exit(main(ARGV))
