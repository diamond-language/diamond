# Sorted set commands. Reopens RedisConnection (see connection.di).
class RedisConnection
  def zadd(key, score, member) = self.command("ZADD", key, score, member)
  def zrem(key, *members) = self.command("ZREM", key, *members)
  def zcard(key) = self.command("ZCARD", key)
  def zincrby(key, amount, member) = self.command("ZINCRBY", key, amount, member)

  # Under RESP2, a String reply (Redis scores are wire-formatted as
  # text regardless of being stored as doubles) -- converted to a real
  # Float here since every score is numeric by definition; a caller
  # never actually wants the raw string. Under RESP3, the reply is
  # already a real Float (protocol.di's own `,` Double decoding) --
  # `Float` has no `#to_f` of its own (confirmed directly: "undefined
  # method 'to_f' for Float"), so calling it unconditionally would
  # crash exactly the callers RESP3 is supposed to serve better, not
  # worse. `nil` (member not in the set) is left as nil either way --
  # nil.to_f() isn't a meaningful thing to ask for.
  def zscore(key, member)
    reply = self.command("ZSCORE", key, member)
    if reply == nil || self.resp3?() then reply else reply.to_f() end
  end

  # Redis's own rank is 0-indexed by ascending score, ties broken by
  # member name -- nil if `member` isn't in the set (matching zscore's
  # own "not present" contract) rather than raising.
  def zrank(key, member) = self.command("ZRANK", key, member)

  # `withscores`: true returns an Array of [member, score: Float] pairs
  # instead of Redis's own raw flat [member1, score1, member2, score2,
  # ...] RESP2 reply -- a caller almost always wants each member paired
  # with its own score, not a flat list they'd have to re-pair
  # themselves (the same reasoning HGETALL's own rebuild in hashes.di
  # already applies). Under RESP3, Redis already sends this same
  # WITHSCORES reply pre-paired (an Array of 2-element Arrays, the
  # score already a real Float via protocol.di's own `,` Double
  # decoding) -- nothing left to rebuild, and re-pairing it anyway
  # would misread each already-Float score as if it were the next
  # member's own name.
  def zrange(key, start = 0, stop = -1, withscores = false)
    if withscores
      reply = self.command("ZRANGE", key, start, stop, "WITHSCORES")
      if self.resp3?()
        return reply
      end
      pairs = []
      index = 0
      while index < reply.length()
        pairs.push([reply[index], reply[index + 1].to_f()])
        index += 2
      end
      pairs
    else
      self.command("ZRANGE", key, start, stop)
    end
  end

  # min/max accept a plain number (an inclusive bound) or Redis's own
  # "-inf"/"+inf" String sentinels for an unbounded end -- passed
  # through to the command exactly as given, no translation needed
  # either way. See #zrange's own comment on `withscores`/RESP3.
  def zrangebyscore(key, min, max, withscores = false)
    if withscores
      reply = self.command("ZRANGEBYSCORE", key, min, max, "WITHSCORES")
      if self.resp3?()
        return reply
      end
      pairs = []
      index = 0
      while index < reply.length()
        pairs.push([reply[index], reply[index + 1].to_f()])
        index += 2
      end
      pairs
    else
      self.command("ZRANGEBYSCORE", key, min, max)
    end
  end
end
