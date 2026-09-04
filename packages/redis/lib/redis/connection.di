# A single Redis connection: a plain blocking TCPSocket (unlike
# packages/gremlin's own non-blocking, Fiber-yielding connections --
# an ordinary app calling a Redis command wants a normal synchronous
# round trip, the same shape SQLite3/PostgreSQL/MariaDB's own native
# drivers already have, not something built for concurrent multiplexing)
# wrapped in the RESP2 encode/decode from protocol.di.
#
# `Redis` itself is a thin factory -- `RedisConnection` (this file) is
# where every actual command method lives, added by every other file in
# this package reopening the class (module/class reopening, confirmed
# working across files this codebase already relies on elsewhere --
# active_record/arel are both split the same "one file per class,
# reopened as needed" way). #command(*args) is the one raw primitive
# every one of those higher-level methods is built on.
class Redis
  # `resp3: true` sends `HELLO 3` right after connecting, switching
  # this connection over to RESP3 for the rest of its life (Redis has
  # no per-command protocol selection -- it's a whole-connection
  # setting). Defaults `false`: every existing caller keeps today's
  # plain RESP2 behavior unchanged, and an older-than-Redis-6 server
  # (no HELLO at all) still works as long as this stays off.
  #
  # `resp3:` is a keyword, but skipping `port`/`options` while naming
  # it still fails to compile ("missing argument") -- the same
  # Diamond keyword-argument gap rule gremlin_serve's own
  # tick_interval/on_tick ran into (packages/gremlin/lib/gremlin/server.di):
  # a keyword can fill any parameter, but only if every default-valued
  # one *before* the highest one actually supplied is also supplied.
  # `Redis.connect("127.0.0.1", resp3: true)` won't compile;
  # `Redis.connect("127.0.0.1", 6379, nil, resp3: true)` (or naming
  # `port`/`options` too) will.
  #
  # A server that doesn't understand `HELLO` (Redis < 6) raises an
  # ordinary RedisError here rather than silently staying on RESP2 --
  # asking for RESP3 and not getting it is worth knowing about, not
  # hiding.
  def self.connect(host, port = 6379, options = nil, resp3 = false)
    conn = RedisConnection.new(TCPSocket.connect(host, port, options), resp3)
    if resp3
      conn.command("HELLO", "3")
    end
    conn
  end
end

class RedisConnection
  def initialize(socket, resp3 = false)
    @socket = socket
    @resp3 = resp3
  end

  # Whether this connection negotiated RESP3 (`Redis.connect(...,
  # resp3: true)`) -- a handful of commands whose reply Redis itself
  # restructures under RESP3 (HGETALL, ZRANGE ... WITHSCORES, ...)
  # check this to know which shape to expect back, rather than
  # inspecting the decoded value's own runtime shape to guess. See
  # hashes.di's own #hgetall and
  # sorted_sets.di's own #zrange/#zrangebyscore.
  def resp3?() = @resp3

  # The raw primitive: sends one RESP2 command (`args` stringified
  # positionally, e.g. `command("SET", "foo", "bar")`) and returns its
  # decoded reply -- an ordinary String/Int/Array/nil, or a raised
  # RedisError for an error reply. Every command-family method
  # (strings.di, hashes.di, ...) is a thin, named wrapper around this;
  # nothing stops a caller reaching for it directly for a command this
  # package hasn't added a dedicated method for yet.
  #
  # Deliberately *not* return-type-annotated (`-> Int`/`-> Bool`/...) on
  # any command method built on it, even though most commands really do
  # have one fixed real reply type -- confirmed directly as a genuine
  # incompatibility, not just an omission: while a connection is inside
  # a MULTI (see transactions.di), every queued command's own reply is
  # the plain String "QUEUED" instead of its real value, and Diamond
  # enforces a declared return type at runtime (`expected Int, got
  # String`) even when the method's own body is a plain passthrough.
  # An annotated `incr` would work fine called normally but raise
  # TypeError the moment it's called from inside a #multi block --
  # exactly the case #multi's own ergonomics depend on working.
  def command(*args)
    @socket.write(redis_encode_command(args))
    redis_read_reply(@socket)
  end

  def close()
    @socket.close()
  end
end
