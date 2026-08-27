def require_conversion(callback: Callable[[Int], String]) -> String
  callback(1)
end

class ConflictingReference
  def self.identity[T](value: T) -> T = value
end

require_conversion(ConflictingReference.identity)
