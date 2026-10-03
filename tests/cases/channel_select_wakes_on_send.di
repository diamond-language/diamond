# No sleep builtin: park on a never-ready channel until a deadline.
def nap(seconds)
  Channel.new(1).wait_readable([], Time.monotonic() + seconds)
end

def late_sender(ch)
  nap(0.05)
  ch.send("late")
end

a = Channel.new(1)
b = Channel.new(1)
t = Thread.new(late_sender, b)
result = Channel.select([a, b])
t.join()
result
