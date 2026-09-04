# Key-generic commands -- apply to a key regardless of what type of
# value it holds (string, hash, list, set, sorted set). Reopens
# RedisConnection (see connection.di's own comment on why this package
# is split this way).
class RedisConnection
  # DEL key [key ...] -> number of keys actually removed (a key that
  # didn't exist doesn't count).
  def del(*keys) = self.command("DEL", *keys)

  # EXISTS key [key ...] -> how many of the given keys exist (which can
  # exceed 1 if a key is passed more than once) -- kept as the real
  # Redis Int reply rather than collapsed to Bool, since it's genuinely
  # count-shaped once more than one key is involved.
  def exists(*keys) = self.command("EXISTS", *keys)

  # The single-key ergonomic case EXISTS itself doesn't distinguish
  # from "exists twice" -- true/false, matching what a caller almost
  # always actually wants to ask.
  def exists?(key) = self.exists(key) > 0

  def expire(key, seconds) = self.command("EXPIRE", key, seconds) == 1
  def persist(key) = self.command("PERSIST", key) == 1

  # -1: the key exists but has no expiry set. -2: the key doesn't exist
  # at all. Both are real, distinct TTL replies, not error conditions --
  # returned as plain Ints, not nil, so a caller can tell them apart.
  def ttl(key) = self.command("TTL", key)

  # "string" | "list" | "set" | "zset" | "hash" | "none" (no such key).
  def type(key) = self.command("TYPE", key)

  # Real Redis's own docs warn KEYS can be slow on a large keyspace (an
  # O(N) full scan that blocks the single-threaded server for its whole
  # duration) -- included anyway since it's the direct, commonly-
  # reached-for way to list matching keys when that cost is acceptable
  # (a small keyspace, an offline/maintenance script, ...). #scan_each
  # below is the safe alternative for a large keyspace.
  def keys(pattern) = self.command("KEYS", pattern)

  def rename(key, new_key) = self.command("RENAME", key, new_key)

  # One SCAN step: `cursor` is "0" to start a fresh iteration, or
  # whatever a previous call's own "cursor" came back with to continue
  # one already in progress. Deliberately not hidden behind #scan_each
  # alone -- SCAN's own resumable-cursor contract (survives being
  # paused and resumed later, unlike a stateful iterator object would)
  # is part of its real value over KEYS, and some callers genuinely
  # want to drive it a step at a time themselves (e.g. spreading a big
  # scan across several request/response cycles instead of one long
  # loop). "0" coming back as the *next* cursor means the iteration is
  # complete -- not "no keys matched this step", which SCAN can
  # legitimately return uncompleted (`keys: []` with a non-"0" cursor
  # is normal mid-scan, not an error or an early "done").
  def scan(cursor, match = nil, count = nil)
    args = ["SCAN", cursor]
    unless match == nil
      args = args.concat(["MATCH", match])
    end
    unless count == nil
      args = args.concat(["COUNT", count])
    end
    reply = self.command(*args)
    {"cursor": reply[0], "keys": reply[1]}
  end

  # The common case: drive #scan to completion, yielding every matched
  # key exactly once (SCAN's own contract already guarantees that much,
  # even though the same key can be returned more than once across
  # separate #scan calls under concurrent modification -- deduplication
  # is this method's own job, not SCAN's). Safe for a large keyspace,
  # unlike #keys -- each step is O(COUNT), not O(N), so the single-
  # threaded server stays responsive to other clients between steps.
  def scan_each(match = nil, count = nil, &block: Callable[1])
    seen = {}
    cursor = "0"
    loop do
      step = self.scan(cursor, match, count)
      # A plain index loop, not step["keys"].each() do |key| ... end --
      # `yield` only reaches the block bound to *this* method's own
      # lexical body; calling it from inside a further nested block
      # (the Callable #each itself invokes) raises "yield outside a
      # fiber" (confirmed directly), unlike the bare `loop do...end`
      # around this whole method, which is inline looping syntax, not a
      # real nested closure boundary.
      keys = step["keys"]
      index = 0
      while index < keys.length()
        key = keys[index]
        unless seen[key]
          seen[key] = true
          yield(key)
        end
        index += 1
      end
      cursor = step["cursor"]
      if cursor == "0"
        break
      end
    end
  end
end
