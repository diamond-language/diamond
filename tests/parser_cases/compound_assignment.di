x = 5
x += 3
puts(x)
x -= 2
puts(x)
x *= 4
puts(x)
x /= 3
puts(x)
x %= 5
puts(x)

a = nil
a ||= 10
puts(a)
a ||= 20
puts(a)

b = 5
b &&= 7
puts(b)
c = false
c &&= 7
puts(c)

class Counter
  def initialize()
    @n = 0
  end
  def bump()
    @n += 1
    @n
  end
end
counter = Counter.new()
puts(counter.bump())
puts(counter.bump())
nil
