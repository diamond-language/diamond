class Point
  def initialize(x, y)
    @x = x
    @y = y
  end
  def sum() = @x + @y
end

a = Channel.new(1)
b = Channel.new(1)
results = []

# Both have room: the first channel in array order wins.
results << Channel.select_send([a, b], "one")

# a is full now, so the value goes to b.
results << Channel.select_send([a, b], "two")

# Neither has room and the deadline is already past: nil without blocking.
results << Channel.select_send([a, b], "three", Time.monotonic() - 1.0)

# A short real deadline expires with nil too.
results << Channel.select_send([a, b], "three", Time.monotonic() + 0.05)

# What was sent is what each channel holds, in order.
results << a.receive()
results << b.receive()

# Receiving makes room again; the freed channel is picked.
results << Channel.select_send([a, b], Point.new(3, 4))
results << a.receive().sum()

# A closed channel is skipped, even when it is first in the array.
a.close()
results << Channel.select_send([a, b], "after close")
results << b.receive()

# Every channel closed: nothing can ever accept the value.
b.close()
results << Channel.select_send([a, b], "never")
results
