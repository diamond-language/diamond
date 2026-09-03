# Hash commands. Reopens RedisConnection (see connection.di).
class RedisConnection
  def hget(key, field) = self.command("HGET", key, field)
  def hset(key, field, value) -> Int = self.command("HSET", key, field, value)
  def hdel(key, *fields) -> Int = self.command("HDEL", key, *fields)
  def hexists(key, field) -> Bool = self.command("HEXISTS", key, field) == 1
  def hlen(key) -> Int = self.command("HLEN", key)
  def hkeys(key) -> Array = self.command("HKEYS", key)
  def hvals(key) -> Array = self.command("HVALS", key)
  def hincrby(key, field, amount) -> Int = self.command("HINCRBY", key, field, amount)
  def hmget(key, *fields) -> Array = self.command("HMGET", key, *fields)

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

  # HGETALL's own reply is a flat [field1, value1, field2, value2, ...]
  # array -- rebuilt into an ordinary Diamond Hash here, since that's
  # what every caller actually wants back, not a flat list they'd have
  # to re-pair themselves.
  def hgetall(key) -> Hash
    flat = self.command("HGETALL", key)
    result = {}
    index = 0
    while index < flat.length()
      result[flat[index]] = flat[index + 1]
      index += 2
    end
    result
  end
end
