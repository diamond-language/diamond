# The wall-clock budget is measured from the start of the process, not from
# each thread's own start: the main program spends 1.0 s of the 1.5 s budget
# before spawning the worker, so the worker is cut off after roughly 0.5 s.
# (With a clock per thread it would run for the full 1.5 s.) The margins are
# wide on purpose: process startup counts against the budget, and under TSan on
# two CPUs it can take a few hundred milliseconds.
def worker()
  started = Time.monotonic()
  index = 0
  begin
    while true
      index = index + 1
    end
  rescue error: ResourceLimitError
    Time.monotonic() - started
  end
end

origin = Time.monotonic()
while Time.monotonic() - origin < 1.0
end
# The main program shares the deadline the worker just hit, so whichever instruction
# it runs next can trip it too (the check runs every few thousand instructions). That
# raises once and is rescued; the second join returns the finished worker's result.
thread = Thread.new(worker)
begin
  elapsed = thread.join()
rescue error: ResourceLimitError
  elapsed = thread.join()
end
puts(elapsed < 1.2)
