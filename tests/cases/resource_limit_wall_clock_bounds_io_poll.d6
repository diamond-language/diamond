# A blocked IO.poll sleeps in the kernel, where the per-instruction wall-clock
# check cannot run. With no fds and no timeout it would block forever; the
# budget bounds the wait and raises the rescuable ResourceLimitError instead.
started = Time.monotonic()
tripped = false
begin
  IO.poll([], [], -1)
rescue error: ResourceLimitError
  tripped = true
end
puts(tripped)
puts(Time.monotonic() - started < 3.0)
