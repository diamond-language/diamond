def recurse_forever(n)
  recurse_forever(n + 1)
end

def infinite_recursion(ch)
  ch.send(1)
  recurse_forever(0)
end

ch = Channel.new(10)
sup = Supervisor.new()
sup.add_child(infinite_recursion, ch)
ch.receive()
ch.receive()
sup.stop()
[sup.restart_count(0) >= 1, sup.last_error(0) != nil]
