# The exact same pool of workers as bench/thread_pool.di, run under a
# Supervisor instead of plain Thread.new -- isolates what supervision
# itself costs over plain threading when nothing ever crashes (a clean
# return ends a supervised child for good, no restart path exercised
# here; see docs/threads.md's Supervisors section). Compare this file's
# per-iteration time against thread_pool.di's own to see the steady-
# state overhead, if any.
def worker(n)
  total = 0
  i = 0
  while i < n
    total = total + i
    i = i + 1
  end
  total
end

def run()
  sup = Supervisor.new()
  index = 0
  while index < 16
    sup.add_child(worker, 50000)
    index = index + 1
  end
  sup.join()          # always nil -- children may restart forever, so
  sup.alive?(0)        # unlike Thread#join, there's no single "the" result
end
run()
