# Copying a value across a Thread/Channel boundary recurses per nesting level,
# so it is bounded (4096 levels) instead of overflowing the C stack: a chain
# 150000 deep, or an Array that contains itself, fails with a normal error
# naming the real cause. Chains inside the bound still copy intact.
def chain(n)
  c = []
  n.times() do |i|
    c = [c]
  end
  c
end
def depth(v)
  d = 0
  while v.length() > 0
    v = v[0]
    d += 1
  end
  d
end
t = Thread.new(chain, 150000)
begin
  t.join()
rescue e
  puts(e.message())
end
# A failed join is spent: joining again re-raises the same error, and the
# thread is not joined a second time when it is cleaned up at exit.
begin
  t.join()
rescue e
  puts(e.message())
end
puts(depth(Thread.new(chain, 4000).join()))
a = []
a.push(a)
begin
  Thread.new(depth, a)
rescue e
  puts(e.message())
end
c = Channel.new(2)
begin
  c.send(chain(5000))
rescue e
  puts(e.message())
end
c.send(chain(100))
puts(depth(c.receive()))
