# respond_to? checks the class's own method table (including private
# methods, which it must report false for) -- not a native/reflection
# probe of every possible receiver kind (see docs/syntax.md).
class Greeter
  def hello() = "hi"
  private def secret() = "shh"
end
g = Greeter.new()
"#{g.respond_to?(:hello)}, #{g.respond_to?(:secret)}, #{g.respond_to?(:frobnicate)}"
