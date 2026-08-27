def identity[T](value: T) -> T = value

class ReturnReferenceBox
  def identity[T](value: T) -> T = value
  def self.identity[T](value: T) -> T = value
end

def top_reference() -> Callable[[Int], Int] = identity
def singleton_reference() -> Callable[[Int], Int] = ReturnReferenceBox.identity
def bound_reference(box: ReturnReferenceBox) -> Callable[[Int], Int] = box.identity

def explicit_top_reference() -> Callable[[Int], Int]
  return identity
end

def explicit_singleton_reference() -> Callable[[Int], Int]
  return ReturnReferenceBox.identity
end

def explicit_bound_reference(box: ReturnReferenceBox) -> Callable[[Int], Int]
  return box.identity
end

def implicit_top_reference() -> Callable[[Int], Int]
  identity
end

def implicit_singleton_reference() -> Callable[[Int], Int]
  ReturnReferenceBox.identity
end

def implicit_bound_reference(box: ReturnReferenceBox) -> Callable[[Int], Int]
  box.identity
end

def nested_references() -> Array[Hash[String, Callable[[Int], Int]]]
  [{"identity": ReturnReferenceBox.identity}]
end

box = ReturnReferenceBox.new()
puts(top_reference()(42))
puts(singleton_reference()(42))
puts(bound_reference(box)(42))
puts(explicit_top_reference()(42))
puts(explicit_singleton_reference()(42))
puts(explicit_bound_reference(box)(42))
puts(implicit_top_reference()(42))
puts(implicit_singleton_reference()(42))
puts(implicit_bound_reference(box)(42))
puts(nested_references()[0]["identity"](42))
