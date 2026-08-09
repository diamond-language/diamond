def make()
 def once()
  yield(7)
 end
 once
end
f = Fiber.new(make())
f.resume(0)
