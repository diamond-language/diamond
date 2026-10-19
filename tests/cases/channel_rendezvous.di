# No sleep builtin: park on a never-ready channel until a deadline.
def pause(seconds)
  Channel.new(1).wait_readable([], Time.monotonic() + seconds)
end

# Channel.new(0): send returns only once a receiver has taken the value.
ch = Channel.new(0)

# Nothing is waiting to receive, so a non-blocking send has nowhere to go.
begin
  ch.try_send(:early)
  puts("try_send without a receiver should have raised")
rescue error: WouldBlockError
  puts("try_send with no receiver: would block")
end
puts("size: #{ch.size()}")

# A sender blocks until the receiver arrives; order and values are kept.
def sender(channel)
  [1, 2, 3].each() do |n|
    channel.send(n)
  end
  channel.close()
  "sent all"
end
worker = Thread.new(sender, ch)
received = []
loop
  value = ch.receive()
  break if value == nil
  received.push(value)
end
puts("received #{received}")
puts(worker.join())

# A handoff is complete when the value is taken: the sender's progress counter
# can never run ahead of what the receiver has consumed.
progress = Channel.new(8)
ack = Channel.new(0)
def counted_sender(ack, progress)
  3.times() do |i|
    ack.send(i)
    progress.send(i)
  end
  ack.close()
end
sender_thread = Thread.new(counted_sender, ack, progress)
taken = 0
loop
  value = ack.receive()
  break if value == nil
  taken += 1
  # the sender may have recorded at most one more send than we have taken
  puts("sends recorded <= taken: #{progress.size() <= taken}")
end
sender_thread.join()

# try_send succeeds when a receiver is parked.
parked = Channel.new(0)
def parked_receiver(channel) = channel.receive()
reader = Thread.new(parked_receiver, parked)
sent = false
until sent
  begin
    parked.try_send("hello")
    sent = true
  rescue error: WouldBlockError
    pause(0.001)
  end
end
puts("parked receiver got #{reader.join()}")

# Closing releases a blocked sender with an error and discards its value.
stuck = Channel.new(0)
def blocked_sender(channel)
  begin
    channel.send("never taken")
    "sent"
  rescue error: IOError
    "closed while sending"
  end
end
blocked = Thread.new(blocked_sender, stuck)
pause(0.05)
stuck.close()
puts(blocked.join())
puts("receive after close: #{stuck.receive().inspect()}")

# Channel.select takes from a rendezvous channel; select_send refuses one.
picked = Channel.new(0)
def select_sender(channel) = channel.send("via select")
feeder = Thread.new(select_sender, picked)
result = Channel.select([picked])
puts("select: #{result}")
feeder.join()
begin
  Channel.select_send([picked], 1)
rescue error: TypeError
  puts("select_send refused a rendezvous channel")
end
puts("done")
