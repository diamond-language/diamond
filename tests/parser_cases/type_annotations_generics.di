def identity[T](value: T) -> T
  value
end

def preserve[T](values: Array[T]) -> Array[T]
  values
end

puts(identity("diamond"))
identity(42)
