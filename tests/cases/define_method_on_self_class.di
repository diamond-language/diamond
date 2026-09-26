# In a class's own def self. method, self.define_method/compile_method act
# on whichever class self is, so a base class can generate methods for
# each subclass it's called on.
class Record
  def initialize()
    @data = {}
  end
  def data() -> Hash = @data
  def self.fields(names)
    names.each() do |name|
      self.define_method(name, self.compile_method(name, [], "self.data()[key]", {"key": name}))
      self.define_method("#{name}=",
        self.compile_method("#{name}=", ["value"], "self.data()[key] = value", {"key": name}))
    end
  end
end
class Book < Record
end
class Author < Record
end
Book.fields(["title"])
Author.fields(["name"])
book = Book.new()
book.title = "Dune"
author = Author.new()
author.name = "Herbert"
[book.title(), author.name(), book.respond_to?(:name), author.respond_to?(:title)]
