# IO.poll with no descriptors and a negative timeout waits forever. Same
# story as a blocked receive: the wait is bounded by the wall-clock budget.
begin
  IO.poll([], [], -1)
rescue error: ResourceLimitError
  puts("poll was cut short: #{error.message()}")
end
exit(0)
