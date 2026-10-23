class Point
  def initialize(x, y)
    @x = x
    @y = y
  end
  def sum() = @x + @y
end

a = Channel.new(4)
b = Channel.new(4)
results = []

# Only b holds a value: select reports b's index with the value.
b.send("from b")
results << Channel.select([a, b])

# Both ready: the first channel in array order wins.
a.send(1)
b.send(2)
results << Channel.select([a, b])
results << Channel.select([a, b])

# A value crossing the channel is rebased like Channel#receive's.
a.send(Point.new(3, 4))
pair = Channel.select([b, a])
results << [pair[0], pair[1].sum()]

# Nothing ready and a deadline already in the past: nil without blocking.
results << Channel.select([a, b], Time.monotonic() - 1.0)

# A short real deadline expires with nil too.
results << Channel.select([a, b], Time.monotonic() + 0.05)

# Closed channels still drain before they stop being selectable.
a.send("last")
a.close()
b.close()
results << Channel.select([a, b])
results << Channel.select([a, b])
results
