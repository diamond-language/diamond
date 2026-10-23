# A field assigned two different concrete classes across independent
# call sites permanently marks it "unknown" (field_type_status 2) via
# record_field_known_type's own merge rule -- confirms the merge is
# real, not just "first write wins" regardless of what comes later.
# `set_a`/`set_b` are two separately typed setters for the *same* field;
# both run before `use` ever reads it, so by the time `use` compiles,
# the field is already poisoned. `use` never becomes eligible; every
# other method here still compiles independently.
class A
  def initialize(n)
    @n = n
  end
  def describe() -> Int = @n
end

class B
  def initialize(n)
    @n = n
  end
  def describe() -> Int = @n * 10
end

class Container
  def set_a(a: A)
    @item = a
  end
  def set_b(b: B)
    @item = b
  end
  def use(n) -> Int
    x = @item
    total = 0
    i = 0
    while i < n
      total = total + x.describe()
      i = i + 1
    end
    total
  end
end

c = Container.new()
c.set_a(A.new(21))
c.set_b(B.new(21))
puts(c.use(50000))
