# Crashes spaced further apart than within_ms never accumulate, so a child
# that crashes three times in a row slowly is restarted each time.
def flaky(tokens)
  begin
    tokens.try_receive()
  rescue error: WouldBlockError
    return "done"
  end
  raise "crash"
end

tokens = Channel.new(4)
tokens.send(1)
tokens.send(2)
tokens.send(3)
sup = Supervisor.new()
sup.restart_policy({"max_restarts": 1, "within_ms": 30, "backoff_ms": 80})
sup.add_child(flaky, tokens)
sup.join()
[sup.failed?(), sup.restart_count(0), sup.alive?(0)]
