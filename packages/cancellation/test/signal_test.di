require "../lib/cancellation"

def run()
  source = Cancellation::Source.new()
  def handler()
    source.cancel()
  end
  Signal.trap("TERM", handler)
  puts("ready")
  begin
    source.token().receive(Channel.new(1))
  rescue error: Cancellation::Cancelled
    puts("cancelled")
  end
end
run()
exit(0)
