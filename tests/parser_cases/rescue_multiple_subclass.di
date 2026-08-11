class Parent
end

class Child < Parent
end

begin
  raise Child.new()
rescue error: String
  puts("string")
rescue error: Parent
  puts("parent")
end
