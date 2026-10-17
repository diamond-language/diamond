# `statement while cond` and `statement until cond`: the condition is tested first, so the statement may never run.
i = 0
i += 1 while i < 5
puts(i)
j = 10
j -= 3 until j < 0
puts(j)
n = 0
n += 1 while false
puts(n)
log = []
k = 0
log.push(log.length()) while log.length() < 4
puts(log.to_s())
def count_down(n: Int) -> Int
  steps = 0
  n -= 2 while n > 0
  steps += 1 until steps >= 3
  n + steps
end
puts(count_down(7))
x = nil
x = 5 while x == nil
puts(x + 1)
total = 0
items = [1, 2, 3]
total += items.pop() while items.length() > 0
puts(total)
a = [3, 1, 2]
squares = []
squares.push(a.shift() * 2) while a.length() > 0
puts(squares.to_s())

class Counter
  attr_reader n: Int
  def initialize()
    @n = 0
  end
  def fill(limit: Int)
    @n += 1 while @n < limit
    self
  end
end
puts(Counter.new().fill(4).n())

count = 0
[1, 2].each() do |v|
  count += 1 while count < 3 * v
end
puts(count)

grow = 0
[1].each() do |v| grow += v end while grow < 5
puts(grow)

def hot(n: Int) -> Int
  s = 0
  s += n while s < 100
  s
end
t = 0
i = 0
i += 1 while i < 20000
puts(i)
k = 0
k += hot(7) while k < 5000
puts(k)
