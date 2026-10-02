# A sibling that never crashes is still restarted when another child
# crashes under :one_for_all; the spinner proves it by finishing on its
# second start.
def spinner(starts)
  starts.send(1)
  return "done" if starts.size() >= 2
  loop
    x = 1 + 1
  end
end

def crasher(gate, starts)
  gate.send(1)
  if gate.size() < 2
    loop
      break if starts.size() >= 1
    end
    raise "boom"
  end
  "done"
end

starts = Channel.new(10)
gate = Channel.new(10)
sup = Supervisor.new(:one_for_all)
sup.add_child(spinner, starts)
sup.add_child(crasher, gate, starts)
sup.join()
[sup.restart_count(0), sup.restart_count(1), sup.last_error(0), sup.last_error(1)]
