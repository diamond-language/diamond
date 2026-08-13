class Box
 def wrap[T](value: T) -> Array[T] = [value]
end
result = Box.new().wrap(42)
puts(result[0])
nil
