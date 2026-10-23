def make()
 def once()
  yield(1)
  99
 end
 once
end
f = Fiber.new(make())
f.resume(0)
f.resume(0)
begin
 f.resume(0)
rescue error: FiberError
 42
end
