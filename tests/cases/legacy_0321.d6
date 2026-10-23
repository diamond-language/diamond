def make_counter()
 def counter()
  i = 0
  loop do
   got = yield(i)
   i = i + got
  end
 end
 counter
end
f = Fiber.new(make_counter())
first = f.resume(0)
second = f.resume(10)
third = f.resume(5)
"#{first}, #{second}, #{third}"
