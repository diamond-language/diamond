class Parent
end

class Child < Parent
end

begin
  raise Child.new()
rescue error: Parent
  error
rescue error: Child
  error
end
