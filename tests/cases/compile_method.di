# ClassName.compile_method(name, params, body_source, bound_values) --
# compiles a method body from a source string at runtime and returns a
# Callable meant for ClassName.define_method (see docs/design.md's
# "Runtime method synthesis" section). Deliberately exercises: an
# already-existing instance (created before the method was installed)
# calling it correctly; two different classes with a same-named method
# not cross-contaminating; a field the class doesn't have rejected
# before installing anything; a syntax error rejected the same way; and
# bound_values threading an already-evaluated value (from a *different*
# class, which body_source itself has no way to name) into the compiled
# body as an extra parameter the caller never supplies.

class Greeter
  attr_accessor name: String

  def initialize(name: String)
    @name = name
  end
end

callable = Greeter.compile_method("greeting", ["prefix"], "prefix + self.name()", {})
Greeter.define_method("greeting", callable)

g = Greeter.new("Ada")
puts(g.greeting("Hello, "))

# Pre-existing instance, created before the method was installed --
# proves dispatch isn't somehow tied to instances allocated after
# define_method ran.
before = Greeter.new("Grace")
callable2 = Greeter.compile_method("shout", [], "self.name().upcase()", {})
Greeter.define_method("shout", callable2)
puts(before.shout())

# A field the class doesn't have -> ArgumentError, no method installed
# (the field-count safety check, not a crash from a bad @field offset).
begin
  Greeter.compile_method("broken", [], "@nonexistent", {})
  puts("no raise")
rescue error: ArgumentError
  puts("ArgumentError raised: field mismatch")
end

# A syntax error in body_source -> ArgumentError, not a crash.
begin
  Greeter.compile_method("broken2", [], "def(", {})
  puts("no raise")
rescue error: ArgumentError
  puts("ArgumentError raised: syntax error")
end

# Two different classes, same method name -- no cross-contamination
# (each compiles into its own isolated satellite program).
class Other
  attr_accessor label: String
  def initialize(label: String)
    @label = label
  end
end

other_callable = Other.compile_method("greeting", [], "\"other: \" + self.label()", {})
Other.define_method("greeting", other_callable)
o = Other.new("X")
puts(o.greeting())
puts(g.greeting("Hi, "))

# bound_values: an already-evaluated value from a class body_source has
# no way to name directly (class references only resolve within the
# program that compiled them -- the synthesized snippet is its own
# separate program). Spliced in as an extra trailing parameter the
# caller of the installed method never supplies.
class Formatter
  def initialize()
  end
  def wrap(text: String) = "[#{text}]"
end

formatter = Formatter.new()
bound_callable = Other.compile_method("boxed_label", [], "formatter.wrap(self.label())",
  {"formatter": formatter})
Other.define_method("boxed_label", bound_callable)
puts(o.boxed_label())

puts("compile_method smoke ok")
