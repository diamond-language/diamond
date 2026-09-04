# Set commands. Reopens RedisConnection (see connection.di).
class RedisConnection
  def sadd(key, *members) = self.command("SADD", key, *members)
  def srem(key, *members) = self.command("SREM", key, *members)
  def smembers(key) = self.command("SMEMBERS", key)
  def sismember(key, member) = self.command("SISMEMBER", key, member) == 1
  def scard(key) = self.command("SCARD", key)
  def sunion(*keys) = self.command("SUNION", *keys)
  def sinter(*keys) = self.command("SINTER", *keys)
  def sdiff(*keys) = self.command("SDIFF", *keys)
end
