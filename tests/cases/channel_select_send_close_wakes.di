def nap(seconds)
  Channel.new(1).wait_readable([], Time.monotonic() + seconds)
end

def late_closer(ch)
  nap(0.05)
  ch.close()
end

a = Channel.new(1)
a.send("full")
t = Thread.new(late_closer, a)
# Closing the only (full) channel wakes the parked select_send with nil.
result = Channel.select_send([a], "x")
t.join()
result
