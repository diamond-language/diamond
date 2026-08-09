class Factory
 def self.empty[T]() -> Array[T] = []
end
result = Factory.empty[Int]()
begin
 result.push("wrong")
rescue error: TypeError
 42
end
