def total(values: Array[Int]) -> Int = values.sum()
def opaque(value) = value
begin
  total(opaque(["a", 1]))
rescue error: TypeError
  error.message()
end
