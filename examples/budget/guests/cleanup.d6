# Rescuing the error is how a program tidies up and exits on its own terms.
# `ensure` runs on the way out, and the exit status is an ordinary 0.
channel = Channel.new(1)
begin
  begin
    channel.receive()
  ensure
    puts("ensure ran")
  end
rescue error: ResourceLimitError
  puts("rescued, finishing normally")
end
exit(0)
