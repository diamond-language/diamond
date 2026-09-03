# packages/redis

A Redis client for [Diamond](https://gitlab.com/dmn9180/diamond), structured
as a real, `facet`-installable package (see
[`docs/packages.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/packages.md)).
Pure Diamond, no new C dependency and no VM changes — unlike
[`SQLite3`/`PostgreSQL`/`MariaDB`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/databases.md),
which need a native C binding because the actual database engine (SQL
parsing, query execution) lives inside that C library, a Redis client
doesn't have that problem: the real work happens on the Redis *server*,
and the client's whole job is RESP protocol encode/decode over an
ordinary TCP socket — the same shape this project's own HTTP client and
[`packages/websocket`](../websocket/README.md) already are. Speaks
RESP2 by default, RESP3 on request (below).

## Install

Same story as every other package here — copy this directory into
another project as `cuts/redis/`, or give it its own git remote and
depend on it via `facet`.

## Use

```ruby
require "/path/to/redis/lib/redis"

conn = Redis.connect("127.0.0.1", 6379)

conn.set("greeting", "hello")
conn.get("greeting")              # => "hello"
conn.get("missing")               # => nil

conn.incr("counter")              # => 1
conn.expire("counter", 60)        # => true
conn.ttl("counter")               # => 59 (or similar)

conn.hset("user:1", "name", "Ada")
conn.hmset("user:1", {"role": "admin", "active": "true"})
conn.hgetall("user:1")            # => {"name": "Ada", "role": "admin", "active": "true"}

conn.rpush("queue", "a", "b", "c")
conn.lrange("queue")              # => ["a", "b", "c"]

conn.sadd("tags", "ruby", "diamond")
conn.sismember("tags", "ruby")    # => true

conn.zadd("leaderboard", 100, "alice")
conn.zadd("leaderboard", 200, "bob")
conn.zrange("leaderboard", 0, -1, true)  # => [["alice", 100.0], ["bob", 200.0]]

conn.close()
```

`Redis.connect(host, port = 6379, options = nil)` opens a plain
blocking `TCPSocket` (the same `options` Hash `TCPSocket.connect`
itself takes — `connect_timeout_ms`/`read_timeout_ms`/`write_timeout_ms`,
see `docs/networking.md`) and returns a `RedisConnection`. Every
command is an ordinary method on it — `#command(*args)` is the one raw
primitive everything else is built on, and is there directly for any
command this package hasn't added a dedicated method for yet:

```ruby
conn.command("LPOS", "queue", "b")   # any command, by name
```

A command that fails (a type mismatch, a syntax error, ...) raises
`RedisError` with the server's own error message — one class for
anything Redis's own server reports as an error reply, the same "one
class per external system" shape `SQLite3Error` already has.

No command method carries a Diamond return-type annotation (`-> Int`,
`-> Bool`, ...), even though most really do have one fixed real reply
type — deliberate, not an oversight: while a connection is inside a
`#multi` block (below), every queued command's own reply is the plain
String `"QUEUED"` instead of its real value, and Diamond enforces a
declared return type at runtime even on a plain passthrough method. An
annotated `incr` would work normally but raise `TypeError` the moment
it's called from inside a transaction.

### Covered commands

- **Strings**: `get`, `set` (with `ex`/`px`/`nx`/`xx`), `setex`,
  `setnx`, `incr`/`decr`/`incrby`/`decrby`, `append`, `strlen`,
  `mget`/`mset`.
- **Keys**: `del`, `exists`/`exists?`, `expire`, `persist`, `ttl`,
  `type`, `keys`, `rename`, `scan`/`scan_each` (below).
- **Hashes**: `hget`/`hset`/`hmset`/`hgetall`, `hdel`, `hexists`,
  `hlen`, `hkeys`/`hvals`, `hincrby`, `hmget`.
- **Lists**: `lpush`/`rpush`, `lpop`/`rpop`, `llen`, `lrange`,
  `lindex`, `lset`, `ltrim`, `lrem`.
- **Sets**: `sadd`/`srem`, `smembers`, `sismember`, `scard`,
  `sunion`/`sinter`/`sdiff`.
- **Sorted sets**: `zadd`/`zrem`, `zcard`, `zscore`, `zrank`,
  `zincrby`, `zrange`/`zrangebyscore` (both with an optional
  `withscores` returning `[member, score: Float]` pairs instead of
  Redis's own flat reply).
- **Pub/Sub**: `RedisConnection#publish`, and a separate
  `RedisSubscriber` class (below).
- **Transactions**: `RedisConnection#multi` (below).

## Scanning a large keyspace

`keys(pattern)` is an O(N) full scan that blocks the (single-threaded)
server for its whole duration — fine for a small keyspace or an
offline script, a real problem for a large one in production.
`scan_each` drives `SCAN`'s own resumable-cursor protocol to
completion instead, each step bounded by `count` rather than the whole
keyspace:

```ruby
conn.scan_each("user:*", 100) do |key|
  puts(key)
end
```

`scan(cursor, match = nil, count = nil)` is the one-step primitive
underneath it, for a caller that wants to drive the cursor itself (e.g.
spreading one big scan across several request/response cycles instead
of one long loop) — returns `{"cursor", "keys"}`; `"0"` coming back as
the *next* cursor means the iteration is complete, not `"keys"` being
empty (SCAN can legitimately return no matches on a given step without
being done).

## Transactions

`MULTI` puts a connection into a mode where every subsequent command is
*queued*, not executed, until `EXEC` runs them all atomically:

```ruby
result = conn.multi() do |tx|
  tx.set("a", "1")
  tx.incr("counter")
end
# result => ["OK", 1] -- EXEC's own reply: each queued command's real
# reply, in order. Whatever tx.set/tx.incr themselves returned inside
# the block ("QUEUED") is not it -- ignore those.
```

A block that raises `DISCARD`s the transaction (so a failure partway
through queueing doesn't leave the connection stuck inside an open
`MULTI` forever) and re-raises. `WATCH` (optimistic locking) isn't
implemented — this is plain `MULTI`/`EXEC` only.

## Pub/Sub

`SUBSCRIBE` puts a connection into a mode where the server only
accepts more `(P)SUBSCRIBE`/`(P)UNSUBSCRIBE` on it — mixing that with
ordinary commands on the same connection would make `GET`/`SET`/etc.
silently stop working the moment anything ever subscribed to
anything. `RedisSubscriber` is a dedicated class for exactly that
role, with its own connection:

```ruby
sub = RedisSubscriber.connect("127.0.0.1", 6379)
sub.subscribe("news")

loop do
  event = sub.receive()
  # {"type": "message", "pattern": nil, "channel": "news", "payload": "..."}
  puts(event["payload"])
end
```

`#receive()` blocks for the next push reply — a subscribe/unsubscribe
confirmation or an actual published message, both normalized to the
same `{"type", "pattern", "channel", "payload"}` shape (`"pattern"` is
`nil` for a plain, non-pattern subscription; a `PSUBSCRIBE`-matched
message carries the pattern that matched). Publish from any ordinary
`RedisConnection`:

```ruby
conn.publish("news", "hello subscribers")  # => number of subscribers that received it
```

## RESP3

`Redis.connect(host, port, options, resp3: true)` sends `HELLO 3` right
after connecting, switching that connection to RESP3 for the rest of
its life (Redis has no per-command protocol selection — it's a
whole-connection setting; needs Redis 6+, since older servers don't
know `HELLO` at all). `RedisConnection#resp3?()` reports whether a
given connection is in RESP3 mode.

```ruby
conn = Redis.connect("127.0.0.1", 6379, nil, resp3: true)
```

Most commands reply exactly the same way in both protocols — RESP3
mainly adds richer *types* for the reply data itself: real booleans
(`#`) instead of `0`/`1` integers, real doubles (`,`, including
`inf`/`-inf`/`nan`) instead of stringified numbers, a proper null (`_`)
unifying RESP2's separate null-bulk-string and null-array forms, plus
maps (`%`), sets (`~`), big numbers (`(`, kept as a `String` — Diamond
has no userland arbitrary-precision integer construction from one),
verbatim strings (`=`), and bulk errors (`!`, raising `RedisError` the
same as an ordinary error reply). All decoded transparently by the same
`redis_read_reply` regardless of protocol — `#command(*args)` needs no
changes at all to benefit.

A handful of commands' reply *shape* genuinely changes under RESP3 —
Redis sends a real Map instead of a flat array for field-value or
member-score pairs. This package's own `hgetall` and `zrange`/
`zrangebyscore` (with `withscores: true`) check `#resp3?()` to know
which shape to expect back, so they return the identical result either
way — no code changes needed at a call site switching a connection to
`resp3: true`.

## What's deliberately out of scope

- **RESP3 pub/sub unification.** RESP3's own killer feature is that
  pub/sub messages arrive as out-of-band push replies interleaved with
  ordinary command replies on the *same* connection, so one connection
  can subscribe and run normal commands at once — not implemented here
  (see the RESP3 section above); `RedisSubscriber`'s own dedicated
  connection is still required regardless of `resp3:`.
- **`WATCH` (optimistic locking) and scripting (`EVAL`/`EVALSHA`).**
  Not implemented — reach for `#command(*args)` directly if you need
  them; nothing about the protocol layer prevents it.
- **Connection pooling, automatic reconnection, Sentinel/Cluster
  support.** One connection, one Redis. Layer pooling/retry on top in
  application code if you need it.

## Testing

`test.sh` has two parts: pure RESP2 protocol-level checks against a
small in-memory fake connection (encoding, every reply type, error
replies, and a truncated/closed-connection reply), then real
command-family and pub/sub checks against a live
`docker.io/library/redis:alpine` container (via `podman`, or `docker`
if `REDIS_CONTAINER_CLI=docker` is set) — not just this package's own
encoder agreeing with its own decoder. The live section is skipped
(not failed) if neither `podman` nor `docker` is on `PATH`.
