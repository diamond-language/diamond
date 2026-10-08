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
# The main program shares the deadline the worker hits, and each VM looks at the clock only
# every 4096 instructions, so where main trips is arbitrary: whichever instruction it runs
# next at a multiple of 4096 after the deadline. It trips once. If that landed on the join,
# or on the print below, nothing would rescue it and the program would exit with
# ResourceLimitError (about one run in 200 under two pinned CPUs). So main spins here, inside
# the rescue, until it trips; the single trip is then always caught.
thread = Thread.new(worker)
begin
  while true
  end
rescue error: ResourceLimitError
end
elapsed = thread.join()
puts(elapsed < 1.2)
