# A Redis client for Diamond -- pure Diamond, no new C dependency and
# no VM changes, unlike SQLite3/PostgreSQL/MariaDB's own native
# integrations (docs/databases.md). Those need a native C binding
# because the actual database engine (SQL parsing, query execution)
# lives inside that C library; a Redis client doesn't have that
# problem at all -- the real work happens on the Redis *server*, and
# the client's whole job is just RESP2 protocol encode/decode over an
# ordinary TCP socket, exactly the same shape packages/http's own HTTP
# client and packages/websocket already are.
#
#   require "/path/to/redis/lib/redis"
#
#   conn = Redis.connect("127.0.0.1", 6379)
#   conn.set("greeting", "hello")
#   conn.get("greeting")       # => "hello"
#   conn.incr("counter")       # => 1
#   conn.hset("user:1", "name", "Ada")
#   conn.hgetall("user:1")     # => {"name": "Ada"}
#   conn.close()
#
#   sub = RedisSubscriber.connect("127.0.0.1", 6379)  # see pubsub.di
#   sub.subscribe("news")
#   event = sub.receive()      # => {"type": "subscribe", "pattern": nil,
#                               #     "channel": "news", "payload": 1}
#
# One file per command family (a real, if informal, Redis convention
# itself -- its own documentation groups commands the same way):
# `protocol` first (RESP2 encode/decode, no Redis semantics at all --
# everything else is built on it), then `connection` (RedisConnection
# itself, #command being the one raw primitive every family below
# wraps), then each data-type family, each reopening RedisConnection to
# add its own methods rather than being a separate class -- one
# connection, one object, every command available on it directly.
# `pubsub` is the one exception: SUBSCRIBE puts a connection into a
# mode where ordinary commands no longer work on it, so that lives on
# its own RedisSubscriber class instead of reopening RedisConnection
# for everything except #publish (an ordinary command, unaffected).
# `transactions` (MULTI/EXEC) comes last since it calls ordinary
# command methods from every family above on the caller's behalf.
require "./redis/protocol"
require "./redis/connection"
require "./redis/keys"
require "./redis/strings"
require "./redis/hashes"
require "./redis/lists"
require "./redis/sets"
require "./redis/sorted_sets"
require "./redis/pubsub"
require "./redis/transactions"
