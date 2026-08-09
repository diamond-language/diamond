module Types
 def self.empty[T]() -> Array[T] = []
end
result = Types.empty[String]()
begin
 result.push(42)
rescue error: TypeError
 42
end
