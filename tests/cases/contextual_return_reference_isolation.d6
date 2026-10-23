class IsolatedReturnReference
  def self.identity[T](value: T) -> T = value
end

def invalid() -> Callable[[Int], Int]
  IsolatedReturnReference.identity
  IsolatedReturnReference.identity
end
