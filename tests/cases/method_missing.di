# #method_missing(name, args) -- called when ordinary instance method
# dispatch finds no method of that name on the receiver's class. `name`
# is a Symbol, `args` an Array of the call's own explicit arguments
# (the receiver itself is not included, matching how method_missing
# already works everywhere else this shape exists). Deliberately scoped
# to ordinary instance method calls only -- not operator overloading,
# not #to_s, not super, not self.-singleton dispatch (see
# docs/design.md's "method_missing" section for why).

class Plain
end

p = Plain.new()
begin
  p.nonexistent(1, 2)
  puts("no raise")
rescue error: NoMethodError
  puts("NoMethodError raised for a class with no method_missing")
end

class Ghost
  def method_missing(name, args)
    "called #{name} with #{args.length()} args"
  end
end

g = Ghost.new()
puts(g.anything())
puts(g.another(1, 2, 3))

# Wrong arity on method_missing itself -> ArgumentError, same as any
# other mis-arity call.
class WrongArity
  def method_missing(name)
    "oops"
  end
end

w = WrongArity.new()
begin
  w.whatever()
  puts("no raise")
rescue error: ArgumentError
  puts("ArgumentError raised for method_missing arity mismatch")
end

# A real, explicitly-defined method always wins over method_missing --
# dispatch never even reaches the fallback for a method that exists.
class Mixed
  def real() = "real method"
  def method_missing(name, args) = "missing: #{name}"
end

m = Mixed.new()
puts(m.real())
puts(m.fake())

# method_missing is NOT consulted for operator overloading or #to_s --
# both keep their own existing native fallback behavior unchanged.
class NoOperator
end

no = NoOperator.new()
begin
  no + 1
  puts("no raise")
rescue error: TypeError
  puts("TypeError raised for a missing + operator, not NoMethodError")
end

puts("method_missing smoke ok")
