# Sorted set commands. Reopens RedisConnection (see connection.di).
class RedisConnection
  def zadd(key, score, member) = self.command("ZADD", key, score, member)
  def zrem(key, *members) = self.command("ZREM", key, *members)
  def zcard(key) = self.command("ZCARD", key)
  def zincrby(key, amount, member) = self.command("ZINCRBY", key, amount, member)

  # A String reply (Redis scores are wire-formatted as text regardless
  # of being stored as doubles), or nil if `member` isn't in the set at
  # all -- converted to a real Float here (nil left as nil, since
  # nil.to_f() isn't a meaningful thing to ask for) since every score is
  # numeric by definition; a caller never actually wants the raw string.
  def zscore(key, member)
    reply = self.command("ZSCORE", key, member)
    if reply == nil then nil else reply.to_f() end
  end

  # Redis's own rank is 0-indexed by ascending score, ties broken by
  # member name -- nil if `member` isn't in the set (matching zscore's
  # own "not present" contract) rather than raising.
  def zrank(key, member) = self.command("ZRANK", key, member)

  # `withscores`: true returns an Array of [member, score: Float] pairs
  # instead of Redis's own raw flat [member1, score1, member2, score2,
  # ...] reply -- a caller almost always wants each member paired with
  # its own score, not a flat list they'd have to re-pair themselves
  # (the same reasoning HGETALL's own rebuild in hashes.di already
  # applies).
  def zrange(key, start = 0, stop = -1, withscores = false)
    if withscores
      flat = self.command("ZRANGE", key, start, stop, "WITHSCORES")
      pairs = []
      index = 0
      while index < flat.length()
        pairs.push([flat[index], flat[index + 1].to_f()])
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
  # either way.
  def zrangebyscore(key, min, max, withscores = false)
    if withscores
      flat = self.command("ZRANGEBYSCORE", key, min, max, "WITHSCORES")
      pairs = []
      index = 0
      while index < flat.length()
        pairs.push([flat[index], flat[index + 1].to_f()])
        index += 2
      end
      pairs
    else
      self.command("ZRANGEBYSCORE", key, min, max)
    end
  end
end
