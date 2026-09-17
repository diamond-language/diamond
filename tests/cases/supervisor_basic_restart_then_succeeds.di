def crashy_then_succeeds(attempts)
  attempts.send(1)
  if attempts.size() < 3
    raise "boom"
  end
  "done"
end

attempts = Channel.new(10)
sup = Supervisor.new()
sup.add_child(crashy_then_succeeds, attempts)
sup.join()
[sup.restart_count(0), sup.alive?(0)]
