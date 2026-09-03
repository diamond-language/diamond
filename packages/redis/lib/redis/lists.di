# List commands. Reopens RedisConnection (see connection.di).
class RedisConnection
  def lpush(key, *values) = self.command("LPUSH", key, *values)
  def rpush(key, *values) = self.command("RPUSH", key, *values)
  def lpop(key) = self.command("LPOP", key)
  def rpop(key) = self.command("RPOP", key)
  def llen(key) = self.command("LLEN", key)
  def lindex(key, index) = self.command("LINDEX", key, index)
  def lset(key, index, value) = self.command("LSET", key, index, value)
  def ltrim(key, start, stop) = self.command("LTRIM", key, start, stop)
  def lrem(key, count, value) = self.command("LREM", key, count, value)

  # 0..-1 (the whole list) is the common case a caller reaches for most
  # -- both bounds default so `lrange(key)` alone already does that,
  # matching Redis's own `-1` "last element" convention rather than
  # needing the list's own length known up front.
  def lrange(key, start = 0, stop = -1) = self.command("LRANGE", key, start, stop)
end
