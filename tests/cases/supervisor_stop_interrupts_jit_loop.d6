# Compiled code looks at the interrupt flag on every backward jump, so a
# supervised worker spinning in a native loop (a while loop, or a function's
# own tail call) is stopped by stop(timeout_ms) like an interpreted one. The
# loops sit in functions the worker calls, since the worker's own entry
# function is interpreted.
def spin()
  i = 0
  while true
    i = i + 1
  end
end

def tail_spin(n)
  return tail_spin(n + 1)
end

def spin_child() = spin()

def tail_child() = tail_spin(0)

sup = Supervisor.new()
sup.add_child(spin_child)
sup.add_child(tail_child)
puts(sup.stop(100))
puts(sup.alive?(0))
puts(sup.alive?(1))
