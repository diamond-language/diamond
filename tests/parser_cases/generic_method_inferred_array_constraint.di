class Box
  def wrap[T](value: T) -> Array[T] = [value]
end

result = Box.new().wrap("diamond")
begin
  result.push(42)
  puts("no error")
rescue error: TypeError
  puts("blocked")
end
nil
