# 20, 40, 80, 80 ms between four crashes and their restarts: the pause doubles
# up to max_backoff_ms, so the whole run takes at least 220 ms (the fixed
# 20 ms delay would be 80).
def flaky(tokens)
  begin
    tokens.try_receive()
  rescue error: WouldBlockError
    return "done"
  end
  raise "crash"
end

tokens = Channel.new(5)
4.times() do |index|
  tokens.send(index)
end
sup = Supervisor.new()
sup.restart_policy({"backoff_ms": 20, "max_backoff_ms": 80})
started = Time.monotonic()
sup.add_child(flaky, tokens)
sup.join()
[sup.restart_count(0), Time.monotonic() - started >= 0.2]
