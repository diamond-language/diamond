def make()
 def once()
  received = yield(1)
  received + 41
 end
 once
end
f = Fiber.new(make())
puts(f.resume(0))
puts(f.resume(2))
nil
