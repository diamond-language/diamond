# Diamond has no built-in Set, so this is a small one on top of a Hash. It
# also shows how far Diamond's generics go: type parameters are declared on
# FUNCTIONS and METHODS (`def add[T](...)`), not on classes, so the Set
# itself stores untyped members and each generic method ties its own
# argument and result types together.
class Set
  def initialize()
    @members = {}
  end

  # Adds `item` and returns the Set, so adds chain. `T` is bound per call from
  # the argument, so `add(1)` and `add("a")` both type-check.
  def add[T](item: T) -> Set
    @members[item] = true
    self
  end

  def include?[T](item: T) -> Bool = @members.key?(item)

  def size() -> Int = @members.length()

  # Members in insertion order (Hash keys keep it), which keeps every report
  # deterministic.
  def to_a() -> Array = @members.keys()
end

# The distinct elements of `items`, first occurrence wins. `Array[T]` in and
# `Array[T]` out is a promise the compiler checks at each call site: unique
# of an `Array[Int]` is an `Array[Int]`, so its elements can be used as Ints.
def unique[T](items: Array[T]) -> Array[T]
  seen = Set.new()
  items.select() do |item|
    next false if seen.include?(item)
    seen.add(item)
    true
  end
end
