# Hash commands. Reopens RedisConnection (see connection.di).
class RedisConnection
  def hget(key, field) = self.command("HGET", key, field)
  def hset(key, field, value) = self.command("HSET", key, field, value)
  def hdel(key, *fields) = self.command("HDEL", key, *fields)
  def hexists(key, field) = self.command("HEXISTS", key, field) == 1
  def hlen(key) = self.command("HLEN", key)
  def hkeys(key) = self.command("HKEYS", key)
  def hvals(key) = self.command("HVALS", key)
  def hincrby(key, field, amount) = self.command("HINCRBY", key, field, amount)
  def hmget(key, *fields) = self.command("HMGET", key, *fields)

  # HSET key field1 value1 field2 value2 ... from a Hash -- the modern
  # (Redis 4+) multi-field form of HSET itself, not the separate
  # deprecated HMSET command.
  def hmset(key, pairs: Hash)
    args = ["HSET", key]
    pairs.keys().each() do |field|
      args = args.concat([field, pairs[field]])
    end
    self.command(*args)
  end

  # Under RESP2, HGETALL's own reply is a flat [field1, value1, field2,
  # value2, ...] array -- rebuilt into an ordinary Diamond Hash here,
  # since that's what every caller actually wants back, not a flat
  # list they'd have to re-pair themselves. Under RESP3 (see
  # connection.di's own #resp3?), Redis sends a real Map reply for
  # this same command instead, which protocol.di's own decoder already
  # turns into a Diamond Hash directly -- nothing left to rebuild.
  def hgetall(key)
    reply = self.command("HGETALL", key)
    if self.resp3?()
      return reply
    end
    result = {}
    index = 0
    while index < reply.length()
      result[reply[index]] = reply[index + 1]
      index += 2
    end
    result
  end
end
