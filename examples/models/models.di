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

def read_json(path: String)
  file = File.open(path, "r")
  begin
    JSON.parse(file.read())
  ensure
    file.close()
  end
end

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
  schema = read_json(args[0])
  Book.fields(schema["Book"])
  Author.fields(schema["Author"])
  Book.readonly("isbn")

  puts("== generated methods")
  probe = Book.new()
  ["title", "title=", "in_print?", "name", "born"].each() do |name|
    puts("  Book responds to #{name}: #{probe.respond_to?(to_sym(name))}")
  end
  puts("  Author responds to born: #{Author.new().respond_to?(:born)}")

  puts("")
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
  puts("== change tracking")
  dune.pages = 896
  dune.title = "Dune (illustrated)"
  dune.title = "Dune"
  puts("  changes: #{dune.changes()}")

  puts("")
  puts("== type checks, read-only fields, validation")
  attempt("set pages to a String") do dune.pages = "many" end
  attempt("change the isbn") do dune.isbn = "000" end
  draft = Book.build({"title": "An unreasonably long working title for a novel", "pages": 0})
  puts("  draft valid? #{draft.valid?()}")
  draft.errors().each() do |problem| puts("    #{problem}") end

  puts("")
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
