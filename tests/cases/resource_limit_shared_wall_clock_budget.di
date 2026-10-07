# The wall-clock budget is measured from the start of the process, not from
# each thread's own start: the main program spends 0.2 s of the 0.3 s budget
# before spawning the worker, so the worker is cut off after roughly 0.1 s.
# (With a clock per thread it would run for the full 0.3 s.)
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
while Time.monotonic() - origin < 0.2
end
elapsed = Thread.new(worker).join()
puts(elapsed < 0.25)
