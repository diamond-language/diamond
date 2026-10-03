# No sleep builtin: park on a never-ready channel until a deadline.
def nap(seconds)
  Channel.new(1).wait_readable([], Time.monotonic() + seconds)
end

def closer(a, b)
  nap(0.05)
  a.close()
  nap(0.05)
  b.close()
end

a = Channel.new(1)
b = Channel.new(1)
t = Thread.new(closer, a, b)
# Blocks with nothing queued; wakes on each close and returns nil only
# when the last channel is closed and drained.
result = Channel.select([a, b])
t.join()
[result, a.closed?(), b.closed?()]
