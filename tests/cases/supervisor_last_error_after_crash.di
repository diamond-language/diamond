def always_raises(ch)
  ch.send(1)
  raise "boom"
end

ch = Channel.new(10)
sup = Supervisor.new()
sup.add_child(always_raises, ch)
ch.receive()
ch.receive()
sup.stop()
[sup.restart_count(0) >= 1, sup.last_error(0)]
