# Inside an instance method, `name(...)` calls self's method when no
# function of that name exists -- including one defined further down, an
# inherited one, a private one, and from inside a block.
def shout(text: String) -> String = text.upcase()

class Base
  def greet(name: String) -> String = "hello #{name}"
end

class Greeter < Base
  def all(names: Array[String]) -> Array[String]
    names.map() do |name| decorate(greet(name)) end
  end

  def loud(name: String) -> String = shout(greet(name))

  private

  def decorate(text: String) -> String = "<#{text}>"
end

g = Greeter.new()
[g.all(["ada", "bo"]), g.loud("cy")]
