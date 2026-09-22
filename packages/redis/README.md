# redis

Connect to Redis using RESP2 or RESP3.

## Installation

Install the cut at `cuts/redis/` and load it with `require_cut "redis"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

## Usage

```ruby
require_cut "redis"

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

## Notes

Call `conn.close()` when finished. `Redis.connect(host, port)` uses RESP2 by default.
