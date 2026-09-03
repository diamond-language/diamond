# MULTI/EXEC transactions. Reopens RedisConnection (see connection.di).
#
# Once MULTI is sent, every command this same connection sends is
# *queued*, not executed -- the server replies "+QUEUED" to each one
# instead of that command's own real reply, until EXEC runs every
# queued command atomically and replies with one Array of their real
# replies, in order (or a null array/nil if the transaction was
# discarded instead of executed). #multi's own block just calls
# ordinary command methods on `tx` (the same RedisConnection,
# passed back to the block for a readable call site) -- each one gets
# back "QUEUED", which the block is expected to ignore, not the real
# value; only #multi's own return value (EXEC's reply) has the real
# ones.
#
# WATCH (optimistic locking -- abort the transaction if a watched key
# changed before EXEC) is not implemented; this is plain MULTI/EXEC
# only.
class RedisConnection
  # A block that raises discards the transaction (so a failure
  # partway through queueing doesn't leave this connection stuck
  # inside an open MULTI forever) and re-raises, rather than
  # attempting EXEC on a possibly-incomplete queue.
  def multi(&block: Callable[1])
    self.command("MULTI")
    begin
      yield(self)
    rescue error: StandardError
      self.command("DISCARD")
      raise error
    end
    self.command("EXEC")
  end
end
