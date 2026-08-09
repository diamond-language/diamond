def make()
 def once(x)
  x
 end
 once
end
begin
 Fiber.new(make())
rescue error: ArgumentError
 42
end
