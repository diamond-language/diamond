# No sleep builtin: park on a never-ready channel until a deadline.
def nap(seconds) = Channel.new(1).wait_readable([], Time.monotonic() + seconds)

def late_receiver(ch)
  nap(0.05)
  ch.receive()
end

a = Channel.new(1)
b = Channel.new(1)
a.send("a full")
b.send("b full")
t = Thread.new(late_receiver, b)
# Both are full, so this parks until the thread drains b.
result = Channel.select_send([a, b], "sent")
t.join()
[result, b.receive()]
