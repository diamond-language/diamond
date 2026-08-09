def make()
 def once()
  yield(1)
  99
 end
 once
end
f = Fiber.new(make())
a = f.resume(0)
b = f.resume(0)
s = f.status()
"#{a}, #{b}, #{s}"
