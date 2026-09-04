#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-redis-package` from the repo root, which sets this up already).
# The live section below also requires `podman` (or `docker` --
# REDIS_CONTAINER_CLI overrides which) to run a real
# docker.io/library/redis:alpine container -- skipped, not failed, if
# neither is on PATH, the same convention packages/websocket's own
# Node-based interop section already uses.
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

# Part 1: pure RESP2 protocol-level checks -- no real server, just
# protocol.di exercised directly against a small in-memory fake
# connection. Anything that doesn't need a real Redis lives here
# rather than in the slower, container-dependent section below.
unit_out="$(mktemp)"
if ! "$diamond" -e "$(cat <<'DIEOF'
require "./lib/redis/protocol"

def check(condition, message)
  unless condition
    raise RuntimeError.new(message)
  end
end

# A fake connection serving #gets()/#read(n) from a fixed input
# buffer, and collecting every #write(value) into its own buffer --
# enough of TCPSocket's own blocking File-shaped contract for
# protocol.di's own needs.
class FakeConn
  def initialize(input)
    @input = input
    @written = StringBuilder.new()
  end
  def gets()
    newline = @input.index_of("\n")
    if newline == nil
      return nil
    end
    line = @input.slice(0, newline)
    if line.length() > 0 && line.slice(line.length() - 1, 1) == "\r"
      line = line.slice(0, line.length() - 1)
    end
    @input = @input.slice(newline + 1, @input.length() - newline - 1)
    line
  end
  # Clamped to what's actually left -- String#slice raises IndexError
  # for a length past the end of the string rather than clamping
  # itself, so asking for more than @input has left (exactly the
  # truncated-reply scenario this fake exists to simulate) needs
  # handling here, not left to slice to reject outright.
  def read(n)
    took = if n > @input.length() then @input.length() else n end
    result = @input.slice(0, took)
    @input = @input.slice(took, @input.length() - took)
    result
  end
  def write(value)
    @written.append(value)
  end
  def written() = @written.to_s()
end

# Command encoding: the RESP2 "multibulk" array-of-bulk-strings form
# every real client sends, regardless of which command it is.
encoded = redis_encode_command(["SET", "foo", "bar"])
check(encoded == "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", "wrong command encoding: #{encoded}")

# Reply decoding: all five RESP2 reply types, plus both of its "no
# value" shapes (a null bulk string and a null array) collapsing to
# the same Diamond nil.
check(redis_read_reply(FakeConn.new("+OK\r\n")) == "OK", "simple string")
check(redis_read_reply(FakeConn.new(":1000\r\n")) == 1000, "integer")
check(redis_read_reply(FakeConn.new("$6\r\nfoobar\r\n")) == "foobar", "bulk string")
check(redis_read_reply(FakeConn.new("$0\r\n\r\n")) == "", "empty bulk string")
check(redis_read_reply(FakeConn.new("$-1\r\n")) == nil, "null bulk string")
check(redis_read_reply(FakeConn.new("*-1\r\n")) == nil, "null array")
check(redis_read_reply(FakeConn.new("*0\r\n")) == [], "empty array")
array_reply = redis_read_reply(FakeConn.new("*3\r\n$3\r\nfoo\r\n:42\r\n$-1\r\n"))
check(array_reply == ["foo", 42, nil], "mixed array: #{array_reply}")
nested_reply = redis_read_reply(FakeConn.new("*2\r\n*2\r\n:1\r\n:2\r\n*1\r\n+ok\r\n"))
check(nested_reply == [[1, 2], ["ok"]], "nested array: #{nested_reply}")

# Error replies raise RedisError with the server's own message, not a
# generic IOError.
error_raised = false
begin
  redis_read_reply(FakeConn.new("-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"))
rescue error: RedisError
  error_raised = true
  check(error.message() == "WRONGTYPE Operation against a key holding the wrong kind of value", "error message: #{error.message()}")
end
check(error_raised, "expected an error reply to raise RedisError")

# A connection that closes mid-reply raises a plain IOError, not a
# silent wrong value -- claims a 10-byte bulk string but the fake
# connection only actually has 9 bytes total left (no trailing CRLF
# either), so #read(10) genuinely can't return the full amount asked
# for.
mid_reply_error = false
begin
  redis_read_reply(FakeConn.new("$10\r\ntoo short"))
rescue error: IOError
  mid_reply_error = true
end
check(mid_reply_error, "expected a truncated bulk string to raise IOError")

# --- RESP3 reply types ---
check(redis_read_reply(FakeConn.new("_\r\n")) == nil, "RESP3 null")
check(redis_read_reply(FakeConn.new("#t\r\n")) == true, "RESP3 boolean true")
check(redis_read_reply(FakeConn.new("#f\r\n")) == false, "RESP3 boolean false")
check(redis_read_reply(FakeConn.new(",3.14\r\n")) == 3.14, "RESP3 double")
check(redis_read_reply(FakeConn.new(",1\r\n")) == 1.0, "RESP3 double, integer-valued")
inf_reply = redis_read_reply(FakeConn.new(",inf\r\n"))
check(inf_reply > 100000000.0, "RESP3 double +inf: #{inf_reply}")
neg_inf_reply = redis_read_reply(FakeConn.new(",-inf\r\n"))
check(neg_inf_reply < -100000000.0, "RESP3 double -inf: #{neg_inf_reply}")
nan_reply = redis_read_reply(FakeConn.new(",nan\r\n"))
check(nan_reply != nan_reply, "RESP3 double nan (NaN is the one value unequal to itself): #{nan_reply}")
check(redis_read_reply(FakeConn.new("(12345678901234567890\r\n")) == "12345678901234567890", "RESP3 big number (kept as a String)")
check(redis_read_reply(FakeConn.new("=15\r\ntxt:hello world\r\n")) == "hello world", "RESP3 verbatim string (4-byte prefix stripped)")
check(redis_read_reply(FakeConn.new("%2\r\n$1\r\na\r\n:1\r\n$1\r\nb\r\n:2\r\n")) == {"a": 1, "b": 2}, "RESP3 map")
check(redis_read_reply(FakeConn.new("~2\r\n:1\r\n:2\r\n")) == [1, 2], "RESP3 set (decoded as an Array)")
check(redis_read_reply(FakeConn.new(">2\r\n+message\r\n+hi\r\n")) == ["message", "hi"], "RESP3 push (decoded as an Array)")

# RESP3 bulk error raises RedisError, same as RESP2's own simple error.
bulk_error_raised = false
begin
  redis_read_reply(FakeConn.new("!20\r\nSYNTAX invalid input\r\n"))
rescue error: RedisError
  bulk_error_raised = true
  check(error.message() == "SYNTAX invalid input", "RESP3 bulk error message: #{error.message()}")
end
check(bulk_error_raised, "expected a RESP3 bulk error to raise RedisError")

puts("all unit checks passed")
DIEOF
)" >"$unit_out" 2>&1; then
    cat "$unit_out"
    rm -f "$unit_out"
    exit 1
fi
grep -q "all unit checks passed" "$unit_out"
rm -f "$unit_out"
echo "redis unit checks passed"

# Part 2: every command family plus pub/sub, against a real, live
# docker.io/library/redis:alpine -- not just this package's own
# encoder agreeing with its own decoder.
container_cli="${REDIS_CONTAINER_CLI:-}"
if [[ -z "$container_cli" ]]; then
    if command -v podman >/dev/null 2>&1; then
        container_cli="podman"
    elif command -v docker >/dev/null 2>&1; then
        container_cli="docker"
    fi
fi
if [[ -z "$container_cli" ]]; then
    echo "neither podman nor docker found on PATH -- skipping live Redis interop test" >&2
    exit 0
fi

container_name="diamond-redis-package-test"
host_port=16399
"$container_cli" rm -f "$container_name" >/dev/null 2>&1 || true
"$container_cli" run --rm -d --name "$container_name" \
    -p "127.0.0.1:$host_port:6379" \
    docker.io/library/redis:alpine >/dev/null
cleanup() {
    "$container_cli" rm -f "$container_name" >/dev/null 2>&1 || true
}
trap cleanup EXIT

ready=0
for _ in $(seq 1 60); do
    if "$container_cli" exec "$container_name" redis-cli ping >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.5
done
if [[ "$ready" != "1" ]]; then
    "$container_cli" logs "$container_name" >&2
    echo "redis container did not become ready" >&2
    exit 1
fi

live_out="$(mktemp)"
if ! "$diamond" -e "$(cat <<DIEOF
require "./lib/redis"

def check(condition, message)
  unless condition
    raise RuntimeError.new(message)
  end
end

conn = Redis.connect("127.0.0.1", $host_port)
conn.command("FLUSHDB")

# --- strings ---
check(conn.set("greeting", "hello") == "OK", "SET")
check(conn.get("greeting") == "hello", "GET")
check(conn.get("missing") == nil, "GET missing key")
check(conn.set("only_if_absent", "first", nil, nil, true) == "OK", "SET NX on an absent key")
check(conn.set("only_if_absent", "second", nil, nil, true) == nil, "SET NX on an existing key")
check(conn.setnx("only_if_absent", "third") == false, "SETNX on an existing key")
check(conn.incr("counter") == 1, "INCR")
check(conn.incrby("counter", 5) == 6, "INCRBY")
check(conn.decrby("counter", 2) == 4, "DECRBY")
check(conn.append("greeting", " world") == 11, "APPEND")
check(conn.strlen("greeting") == 11, "STRLEN")
conn.mset({"m1": "a", "m2": "b"})
check(conn.mget("m1", "m2", "missing") == ["a", "b", nil], "MGET")

# --- keys ---
check(conn.exists?("greeting"), "EXISTS true")
check(!conn.exists?("truly_missing"), "EXISTS false")
check(conn.expire("greeting", 100), "EXPIRE")
check(conn.ttl("greeting") > 0, "TTL after EXPIRE")
check(conn.persist("greeting"), "PERSIST")
check(conn.ttl("greeting") == -1, "TTL after PERSIST")
check(conn.ttl("truly_missing") == -2, "TTL on a missing key")
check(conn.type("counter") == "string", "TYPE")

# --- hashes ---
conn.hset("h", "f1", "v1")
conn.hmset("h", {"f2": "v2", "f3": "v3"})
check(conn.hgetall("h") == {"f1": "v1", "f2": "v2", "f3": "v3"}, "HGETALL")
check(conn.hexists("h", "f1"), "HEXISTS true")
check(conn.hdel("h", "f1") == 1, "HDEL")
check(!conn.hexists("h", "f1"), "HEXISTS false after HDEL")
check(conn.hmget("h", "f2", "f3", "missing") == ["v2", "v3", nil], "HMGET")

# --- lists ---
conn.rpush("l", "a", "b", "c")
check(conn.lrange("l") == ["a", "b", "c"], "LRANGE")
check(conn.llen("l") == 3, "LLEN")
check(conn.lpop("l") == "a", "LPOP")
check(conn.rpop("l") == "c", "RPOP")
check(conn.lindex("l", 0) == "b", "LINDEX")

# --- sets ---
conn.sadd("s", "x", "y", "z")
check(conn.scard("s") == 3, "SCARD")
check(conn.sismember("s", "x"), "SISMEMBER true")
check(!conn.sismember("s", "nope"), "SISMEMBER false")
check(conn.srem("s", "x") == 1, "SREM")

# --- sorted sets ---
conn.zadd("z", 1, "one")
conn.zadd("z", 2, "two")
conn.zadd("z", 3, "three")
check(conn.zrange("z") == ["one", "two", "three"], "ZRANGE")
check(conn.zrange("z", 0, -1, true) == [["one", 1.0], ["two", 2.0], ["three", 3.0]], "ZRANGE WITHSCORES")
check(conn.zscore("z", "two") == 2.0, "ZSCORE")
check(conn.zscore("z", "missing") == nil, "ZSCORE on a missing member")
check(conn.zrank("z", "three") == 2, "ZRANK")
check(conn.zcard("z") == 3, "ZCARD")

# --- errors ---
conn.set("not_a_list", "x")
error_raised = false
begin
  conn.lrange("not_a_list")
rescue error: RedisError
  error_raised = true
  check(error.message().start_with?("WRONGTYPE"), "expected a WRONGTYPE error, got: #{error.message()}")
end
check(error_raised, "expected a type-mismatched command to raise RedisError")

# --- scan ---
scan_index = 0
while scan_index < 25
  conn.set("scan:#{scan_index}", "v")
  scan_index += 1
end
conn.set("noscan:1", "v")
first_step = conn.scan("0", "scan:*", 5)
check(first_step["cursor"] != nil, "SCAN returns a cursor")
check(first_step["keys"].length() <= 25, "SCAN's own COUNT hint keeps one step bounded")
found_keys = []
conn.scan_each("scan:*", 5) do |key|
  found_keys.push(key)
end
check(found_keys.length() == 25, "scan_each found all 25 matching keys, found #{found_keys.length()}")
check(!found_keys.include?("noscan:1"), "scan_each respected the MATCH pattern")

# --- transactions ---
conn.set("tx_counter", "0")
tx_result = conn.multi() do |tx|
  tx.set("tx_key", "hello")
  tx.incr("tx_counter")
  tx.incr("tx_counter")
end
check(tx_result == ["OK", 1, 2], "MULTI/EXEC returns each queued command's own real reply, got #{tx_result}")
check(conn.get("tx_key") == "hello", "a command queued inside MULTI actually applied")
check(conn.get("tx_counter") == "2", "MULTI/EXEC applied both queued INCRs")

tx_error_raised = false
begin
  conn.multi() do |tx|
    tx.set("never_committed", "x")
    raise RuntimeError.new("boom")
  end
rescue error: RuntimeError
  tx_error_raised = true
end
check(tx_error_raised, "expected the block's own exception to propagate out of #multi")
check(conn.get("never_committed") == nil, "a block that raises must DISCARD, not partially commit")
check(conn.command("PING") == "PONG", "the connection must still work normally after a DISCARDed transaction")

# --- RESP3 ---
conn3 = Redis.connect("127.0.0.1", $host_port, nil, true)
check(conn3.resp3?(), "expected a resp3: true connection to report resp3?() == true")
check(!conn.resp3?(), "expected the ordinary RESP2 connection to report resp3?() == false")
conn3.command("FLUSHDB")
conn3.hset("h3", "f1", "v1")
conn3.hset("h3", "f2", "v2")
check(conn3.hgetall("h3") == {"f1": "v1", "f2": "v2"}, "HGETALL under RESP3 (a real Map reply, not a flat Array)")
conn3.zadd("z3", 1, "one")
conn3.zadd("z3", 2, "two")
check(conn3.zrange("z3", 0, -1, true) == [["one", 1.0], ["two", 2.0]], "ZRANGE WITHSCORES under RESP3 (already paired, score already a real Float)")
check(conn3.zscore("z3", "one") == 1.0, "ZSCORE under RESP3 (already a real Float)")
check(conn3.zscore("z3", "missing") == nil, "ZSCORE under RESP3 on a missing member")
config = conn3.command("CONFIG", "GET", "maxmemory")
check(config["maxmemory"] != nil, "CONFIG GET decodes as a real Hash under RESP3, got #{config}")
conn3.close()

conn.close()
puts("all live checks passed")
DIEOF
)" >"$live_out" 2>&1; then
    cat "$live_out"
    rm -f "$live_out"
    exit 1
fi
grep -q "all live checks passed" "$live_out"
rm -f "$live_out"
echo "redis live command checks passed"

# --- pub/sub: a real subscriber process and a real publisher process,
# not just one connection talking to itself.
sub_out="$(mktemp)"
timeout 10 "$diamond" -e "$(cat <<DIEOF
require "./lib/redis"
sub = RedisSubscriber.connect("127.0.0.1", $host_port)
sub.subscribe("news")
sub.receive() # subscribe confirmation
event = sub.receive()
puts("#{event["type"]}|#{event["channel"]}|#{event["payload"]}")
sub.close()
DIEOF
)" >"$sub_out" 2>&1 &
sub_pid=$!
sleep 0.5

"$diamond" -e "$(cat <<DIEOF
require "./lib/redis"
conn = Redis.connect("127.0.0.1", $host_port)
result = conn.publish("news", "hello subscribers")
if result != 1
  raise RuntimeError.new("expected exactly 1 subscriber to receive the message, got #{result}")
end
conn.close()
DIEOF
)"

wait "$sub_pid"
sub_status=$?
if [[ "$sub_status" != "0" ]]; then
    cat "$sub_out" >&2
    exit 1
fi
# `-e` also prints its own final expression's value (here, sub.close()'s
# own nil) after whatever the script explicitly puts() -- only the
# first line is the actual event line this checks.
sub_result="$(head -1 "$sub_out")"
rm -f "$sub_out"
if [[ "$sub_result" != "message|news|hello subscribers" ]]; then
    echo "FAIL: unexpected pub/sub result: $sub_result" >&2
    exit 1
fi
echo "redis pub/sub check passed"
