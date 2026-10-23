def rejects(ch)
  ch.send(1)
  raise ArgumentError.new("bad input")
end

ch = Channel.new(10)
sup = Supervisor.new()
sup.add_child(rejects, ch)
ch.receive()
ch.receive()
sup.stop()
sup.last_error(0)
