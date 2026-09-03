# String commands. Reopens RedisConnection (see connection.di).
class RedisConnection
  def get(key) = self.command("GET", key)

  # `ex`/`px` (seconds/milliseconds until expiry) and `nx`/`xx` ("only
  # set if the key doesn't/does already exist") mirror SET's own real
  # optional arguments -- omit all four (the default) for a plain,
  # unconditional SET. Returns the reply as-is: "OK" on success, or
  # `nil` specifically when `nx`/`xx` was given and the condition
  # wasn't met (Redis's own real behavior -- not an error).
  def set(key, value, ex = nil, px = nil, nx = false, xx = false)
    args = ["SET", key, value]
    unless ex == nil
      args = args.concat(["EX", ex])
    end
    unless px == nil
      args = args.concat(["PX", px])
    end
    if nx
      args = args.concat(["NX"])
    end
    if xx
      args = args.concat(["XX"])
    end
    self.command(*args)
  end

  def setex(key, seconds, value) = self.command("SETEX", key, seconds, value)
  def setnx(key, value) -> Bool = self.command("SETNX", key, value) == 1

  def incr(key) -> Int = self.command("INCR", key)
  def decr(key) -> Int = self.command("DECR", key)
  def incrby(key, amount) -> Int = self.command("INCRBY", key, amount)
  def decrby(key, amount) -> Int = self.command("DECRBY", key, amount)

  def append(key, value) -> Int = self.command("APPEND", key, value)
  def strlen(key) -> Int = self.command("STRLEN", key)

  def mget(*keys) -> Array = self.command("MGET", *keys)

  # MSET key1 value1 key2 value2 ... from an ordinary Diamond Hash --
  # the natural shape to hand this in as, rather than a caller having
  # to flatten it into alternating positional arguments themselves.
  def mset(pairs: Hash)
    args = ["MSET"]
    pairs.keys().each() do |key|
      args = args.concat([key, pairs[key]])
    end
    self.command(*args)
  end
end
