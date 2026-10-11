# Same for a channel wait with nothing ever sent: receive blocks until the
# wall-clock budget raises ResourceLimitError.
channel = Channel.new(1)
started = Time.monotonic()
tripped = false
begin
  channel.receive()
rescue error: ResourceLimitError
  tripped = true
end
puts(tripped)
puts(Time.monotonic() - started < 3.0)
