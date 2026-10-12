# DIAMOND_MAX_LIVE_THREADS=2 lowers the live-thread cap below the built-in 64.
def waiter(channel)
  channel.receive()
end
channel = Channel.new(4)
first = Thread.new(waiter, channel)
second = Thread.new(waiter, channel)
begin
  Thread.new(waiter, channel)
  puts("third started")
rescue error: ThreadError
  puts("third refused: #{error.message()}")
end
channel.send(1)
channel.send(2)
first.join()
second.join()
puts("done")
