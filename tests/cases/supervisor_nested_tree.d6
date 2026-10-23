def inner_worker(ch)
  ch.send("inner ran")
  "inner done"
end

def outer_worker(result_ch)
  inner_sup = Supervisor.new()
  inner_ch = Channel.new(4)
  inner_sup.add_child(inner_worker, inner_ch)
  inner_sup.join()
  result_ch.send(inner_ch.receive())
  "outer done"
end

result_ch = Channel.new(4)
sup = Supervisor.new()
sup.add_child(outer_worker, result_ch)
sup.join()
result_ch.receive()
