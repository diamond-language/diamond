# Reopening a class doesn't loosen the existing "no silent redefinition"
# rule (compiler.c's own duplicate-method check, unmodified by
# reopening) -- confirms this stays a compile error, not the Ruby-style
# silent override reopening could plausibly have been mistaken for.
class Foo
  def bar() = 1
end

class Foo
  def bar() = 2
end
