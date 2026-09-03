# Key-generic commands -- apply to a key regardless of what type of
# value it holds (string, hash, list, set, sorted set). Reopens
# RedisConnection (see connection.di's own comment on why this package
# is split this way).
class RedisConnection
  # DEL key [key ...] -> number of keys actually removed (a key that
  # didn't exist doesn't count).
  def del(*keys) -> Int = self.command("DEL", *keys)

  # EXISTS key [key ...] -> how many of the given keys exist (which can
  # exceed 1 if a key is passed more than once) -- kept as the real
  # Redis Int reply rather than collapsed to Bool, since it's genuinely
  # count-shaped once more than one key is involved.
  def exists(*keys) -> Int = self.command("EXISTS", *keys)

  # The single-key ergonomic case EXISTS itself doesn't distinguish
  # from "exists twice" -- true/false, matching what a caller almost
  # always actually wants to ask.
  def exists?(key) -> Bool = self.exists(key) > 0

  def expire(key, seconds) -> Bool = self.command("EXPIRE", key, seconds) == 1
  def persist(key) -> Bool = self.command("PERSIST", key) == 1

  # -1: the key exists but has no expiry set. -2: the key doesn't exist
  # at all. Both are real, distinct TTL replies, not error conditions --
  # returned as plain Ints, not nil, so a caller can tell them apart.
  def ttl(key) -> Int = self.command("TTL", key)

  # "string" | "list" | "set" | "zset" | "hash" | "none" (no such key).
  def type(key) -> String = self.command("TYPE", key)

  # Real Redis's own docs warn KEYS can be slow on a large keyspace
  # (an O(N) full scan) -- included anyway since it's the direct,
  # commonly-reached-for way to list matching keys; SCAN's cursor-based
  # incremental alternative is out of scope for this first version.
  def keys(pattern) -> Array = self.command("KEYS", pattern)

  def rename(key, new_key) = self.command("RENAME", key, new_key)
end
