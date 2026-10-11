# More than max_restarts crashes inside the window: the supervisor stops
# restarting, interrupts its other children, and join raises.
def crasher()
  raise "boom"
end

def sleeper()
  loop do
  end
end

sup = Supervisor.new()
sup.restart_policy({"max_restarts": 3, "within_ms": 10000, "backoff_ms": 20})
sup.add_child(crasher)
sup.add_child(sleeper)
joined = "returned"
begin
  sup.join()
rescue error: SupervisorError
  joined = "raised"
end
[joined, sup.failed?(), sup.restart_count(0), sup.alive?(0), sup.alive?(1),
 sup.last_error(1)]
