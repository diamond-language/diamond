# Set commands. Reopens RedisConnection (see connection.di).
class RedisConnection
  def sadd(key, *members) -> Int = self.command("SADD", key, *members)
  def srem(key, *members) -> Int = self.command("SREM", key, *members)
  def smembers(key) -> Array = self.command("SMEMBERS", key)
  def sismember(key, member) -> Bool = self.command("SISMEMBER", key, member) == 1
  def scard(key) -> Int = self.command("SCARD", key)
  def sunion(*keys) -> Array = self.command("SUNION", *keys)
  def sinter(*keys) -> Array = self.command("SINTER", *keys)
  def sdiff(*keys) -> Array = self.command("SDIFF", *keys)
end
