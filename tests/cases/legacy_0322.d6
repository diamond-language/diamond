def make()
 def once()
  1
 end
 once
end
f = Fiber.new(make())
before = f.status()
a = f.resume(0)
after = f.status()
alive_before = f.alive?()
"#{before}, #{a}, #{after}, #{alive_before}"
