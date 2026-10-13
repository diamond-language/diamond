# A worker parked in Channel#receive (or #send on a full channel) holds the
# join until stop(timeout_ms) interrupts it.
def receiver()
  Channel.new(1).receive()
end

def sender()
  full = Channel.new(1)
  full.send(1)
  full.send(2)
end

def waiter()
  Channel.new(1).wait_readable([], nil)
end

sup = Supervisor.new()
sup.add_child(receiver)
sup.add_child(sender)
sup.add_child(waiter)
puts(sup.stop(100))
puts(sup.alive?(0))
puts(sup.alive?(1))
puts(sup.alive?(2))
puts(sup.last_error(0))
