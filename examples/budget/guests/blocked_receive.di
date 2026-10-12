# Nothing ever sends on this channel, so receive() would block forever. The
# blocked thread is asleep in the kernel, where a per-instruction check never
# runs; the budget bounds the wait itself and raises ResourceLimitError.
channel = Channel.new(1)
begin
  channel.receive()
rescue error: ResourceLimitError
  puts("receive was cut short: #{error.message()}")
end
exit(0)
