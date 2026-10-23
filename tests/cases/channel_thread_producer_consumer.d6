class Point
  def initialize(x, y)
    @x = x
    @y = y
  end
  def sum() = @x + @y
end

def producer(ch)
  ch.send("hello from thread")
  ch.send(42)
  ch.send(Point.new(3, 4))
  ch.close()
end

ch = Channel.new(4)
t = Thread.new(producer, ch)
first = ch.receive()
second = ch.receive()
third = ch.receive().sum()
fourth = ch.receive()
t.join()
[first, second, third, fourth]
