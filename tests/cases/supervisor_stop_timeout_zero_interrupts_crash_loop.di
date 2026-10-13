# stop(0) interrupts immediately, including a child that is mid-backoff.
def crash()
  raise "boom"
end

sup = Supervisor.new()
sup.restart_policy({"backoff_ms": 500})
sup.add_child(crash)
sup.stop(0)
puts(sup.alive?(0))
