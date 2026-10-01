def send_ready(channel, gate)
  gate.receive()
  channel.send(42)
end

def drain_ready(channel, gate)
  gate.receive()
  channel.receive()
end

def close_ready(channel, gate)
  gate.receive()
  channel.close()
end

channel = Channel.new(1)
gate = Channel.new(1)
worker = Thread.new(send_ready, channel, gate)
gate.send(true)
started = Time.monotonic()
channel.wait_readable([], started + 10.0)
read_woke = Time.monotonic() - started < 5.0
value = channel.receive()
worker.join()

channel.send(1)
worker = Thread.new(drain_ready, channel, gate)
gate.send(true)
started = Time.monotonic()
channel.wait_writable([], started + 10.0)
write_woke = Time.monotonic() - started < 5.0
channel.try_send(2)
worker.join()
channel.receive()

cancel = Channel.new(1)
worker = Thread.new(close_ready, cancel, gate)
gate.send(true)
started = Time.monotonic()
channel.wait_readable([cancel, cancel], started + 10.0)
cancel_woke = Time.monotonic() - started < 5.0
worker.join()

# Already closed notification sources and target channels cannot lose wakeups.
channel.wait_readable([cancel], nil)
channel.close()
channel.wait_readable([], nil)
channel.wait_writable([], nil)

# Deadline-only waits unregister cleanly, including duplicate registrations.
empty = Channel.new(1)
other = Channel.new(1)
200.times() do |i|
  empty.wait_readable([other, other], Time.monotonic())
end
started = Time.monotonic()
empty.wait_readable([], started + 0.02)
deadline_waited = Time.monotonic() >= started + 0.02

errors = 0
begin
  empty.wait_readable([1], nil)
rescue error: TypeError
  errors += 1
end
begin
  empty.wait_writable([], "bad")
rescue error: TypeError
  errors += 1
end
[read_woke, write_woke, cancel_woke, value, deadline_waited, errors]
