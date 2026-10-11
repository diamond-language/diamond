# A program that rescues the budget and then blocks again is stopped when the
# grace allowance ends, not left sleeping in IO.poll forever.
begin
  IO.poll([], [], -1)
rescue error: ResourceLimitError
  nil
end
IO.poll([], [], -1)
