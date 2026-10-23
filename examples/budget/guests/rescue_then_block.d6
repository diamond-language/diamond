# The budget raises once. A program that rescues the error and then blocks
# again is out of line: it only gets the grace allowance, then the process is
# stopped by an error no rescue clause can match.
channel = Channel.new(1)
begin
  channel.receive()
rescue error: ResourceLimitError
  puts("rescued once")
end
channel.receive()
puts("unreachable")
