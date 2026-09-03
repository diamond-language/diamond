# RESP2/RESP3 (REdis Serialization Protocol) wire encode/decode -- the
# one thing every Redis command shares, regardless of which data type
# it operates on.
#
# A command is always sent the same way regardless of protocol version
# or which command it is -- a RESP Array of Bulk Strings (the
# "multibulk" form real clients use, as opposed to Redis's older
# inline-command wire format, which this package never emits):
# `SET foo bar` becomes `*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n`.
# Only *replies* differ by protocol version -- decoded generically here
# by redis_read_reply, which doesn't know or care which command
# produced what it's reading.
#
# RESP3 is opt-in per connection (`Redis.connect(..., resp3: true)`,
# connection.di) via `HELLO 3`; a server that never received that
# always speaks plain RESP2, so redis_read_reply supporting both isn't
# a live ambiguity at read time -- whichever one the connection
# actually negotiated is the only one a compliant server will ever
# send on it. What RESP3 does NOT get here: the one thing it's
# actually *for* -- pub/sub messages arriving as out-of-band `>` push
# replies interleaved with ordinary command replies on the very same
# connection, letting one connection do both instead of needing a
# dedicated subscriber connection. This package still requires
# RedisSubscriber's own separate connection for that (pubsub.di) --
# #command below has no logic to recognize and set aside an
# unsolicited push reply arriving ahead of the one it's actually
# waiting for, so mixing SUBSCRIBE into an ordinary RESP3 connection's
# traffic would misread a push message as if it were the next
# command's own reply. A deliberate scope cut, not a bug: nothing in
# this package ever sends SUBSCRIBE on anything but a dedicated
# RedisSubscriber connection, so the ambiguity never actually arises
# here.

# One error class for anything Redis's own server reports back as an
# error reply (`-ERR ...`, `-WRONGTYPE ...`, RESP3's own bulk-error
# `!...` included) -- matching SQLite3Error's own "one class per
# external system" precedent (docs/databases.md) rather than trying to
# model Redis's own informal error-code-prefix convention as distinct
# Diamond exception types.
class RedisError < StandardError
end

def redis_encode_command(args)
  sb = StringBuilder.new()
  sb.append("*#{args.length()}\r\n")
  index = 0
  while index < args.length()
    arg = "#{args[index]}"
    sb.append("$#{arg.length()}\r\n")
    sb.append(arg)
    sb.append("\r\n")
    index += 1
  end
  sb.to_s()
end

# Exactly `length` bytes, or a clean IOError if the connection closes
# first -- `conn.read(length)` (a plain blocking socket, unlike
# packages/gremlin's own non-blocking connections) already blocks until
# it has them all or hits EOF, so this only needs to check which one
# actually happened, not retry-loop itself.
def redis_read_exact(conn, length)
  if length == 0
    return ""
  end
  data = conn.read(length)
  if data.length() != length
    raise IOError.new("Redis connection closed mid-reply")
  end
  data
end

# The shared shape behind every length-prefixed reply body (RESP2's
# own Bulk String `$`, and RESP3's Bulk Error `!` and Verbatim String
# `=`): `length` bytes of raw content, then a bare trailing CRLF.
def redis_read_length_prefixed_body(conn, length)
  data = redis_read_exact(conn, length)
  conn.gets() # the body's own trailing CRLF
  data
end

# Reads and decodes exactly one RESP reply -- recursively, for every
# container type (Array, RESP3's own Map/Set/Push), since a
# container's own elements are themselves complete RESP values
# (including further containers, e.g. a transaction's own EXEC reply:
# one array of each queued command's own reply).
#
# `nil` is RESP's own encoding for "no value" in three shapes: RESP2's
# null bulk string (`$-1\r\n`, e.g. GET on a missing key), RESP2's null
# array (`*-1\r\n`, e.g. BLPOP timing out), and RESP3's own unified
# Null (`_\r\n`, replacing both of the above once negotiated) -- all
# three collapse to the same Diamond `nil`, matching how callers
# already treat "not found" as nil everywhere else in this ecosystem
# (ActiveRecord's own #find, Hash lookups, ...).
def redis_read_reply(conn)
  line = conn.gets()
  if line == nil
    raise IOError.new("Redis connection closed while reading a reply")
  end
  reply_type = line[0]
  rest = line.slice(1, line.length() - 1)
  if reply_type == "+"
    rest
  elsif reply_type == "-"
    raise RedisError.new(rest)
  elsif reply_type == ":"
    rest.to_i()
  elsif reply_type == "$"
    length = rest.to_i()
    if length == -1
      nil
    else
      redis_read_length_prefixed_body(conn, length)
    end
  elsif reply_type == "*"
    count = rest.to_i()
    if count == -1
      nil
    else
      values = []
      index = 0
      while index < count
        values.push(redis_read_reply(conn))
        index += 1
      end
      values
    end
  elsif reply_type == "_"
    # RESP3 Null: the whole reply is just the two-byte type+CRLF
    # already consumed by `conn.gets()` above -- nothing further to
    # read.
    nil
  elsif reply_type == "#"
    rest == "t"
  elsif reply_type == ","
    # Redis wire-formats these three as literal words rather than a
    # value `to_f()` could parse -- and Diamond's own String#to_f
    # deliberately parses "inf"/"nan" as 0.0 (docs/collections.md), so
    # they need handling before ever reaching it. 1.0/0.0 and 0.0/0.0
    # are real IEEE 754 float operations here, not integer division
    # (which does raise) -- confirmed elsewhere in this codebase's own
    # test suite (tests/cases/spaceship_nan_and_incomparable.di) that
    # 0.0/0.0 produces a real NaN without raising.
    if rest == "inf"
      1.0 / 0.0
    elsif rest == "-inf"
      -1.0 / 0.0
    elsif rest == "nan"
      0.0 / 0.0
    else
      rest.to_f()
    end
  elsif reply_type == "("
    # RESP3 Big Number: an arbitrary-precision integer, wire-formatted
    # as a bare decimal string. Diamond has no userland way to build an
    # arbitrary-precision integer value from a String (bignum support
    # is a VM-internal overflow behavior on ordinary Int arithmetic,
    # docs/design.md -- not something this package can reach into), so
    # this stays a String rather than silently truncating precision by
    # forcing it through `to_i()`. None of the commands this package
    # covers ever actually return one; only here so a reply carrying
    # this type decodes instead of raising "unrecognized RESP reply
    # type".
    rest
  elsif reply_type == "!"
    # RESP3 Bulk Error: same length-prefixed shape as Bulk String, but
    # -- like RESP2's own simple `-` error -- raises RedisError rather
    # than returning the text as a value.
    length = rest.to_i()
    raise RedisError.new(redis_read_length_prefixed_body(conn, length))
  elsif reply_type == "="
    # RESP3 Verbatim String: identical wire shape to Bulk String, plus
    # a mandatory 4-byte type prefix ("txt:", "mkd:", ...) ahead of the
    # actual content -- stripped here, since nothing in this package
    # distinguishes verbatim-string sub-formats from an ordinary
    # string.
    length = rest.to_i()
    body = redis_read_length_prefixed_body(conn, length)
    body.slice(4, body.length() - 4)
  elsif reply_type == "%"
    # RESP3 Map: `rest` is the number of key-VALUE *pairs*, so twice as
    # many individual RESP values follow as a same-count Array would
    # have -- decoded into an ordinary Diamond Hash. This is the one
    # RESP3 addition genuinely load-bearing for this package: several
    # commands (HGETALL, ZRANGE ... WITHSCORES, ...) reply with a flat
    # Array under RESP2 but a real Map under RESP3 -- see hashes.di's
    # own #hgetall and sorted_sets.di's own #zrange/#zrangebyscore for
    # where that shape difference actually matters to a caller.
    count = rest.to_i()
    result = {}
    index = 0
    while index < count
      key = redis_read_reply(conn)
      value = redis_read_reply(conn)
      result[key] = value
      index += 1
    end
    result
  elsif reply_type == "~"
    # RESP3 Set: wire-identical to Array, semantically a set (no
    # duplicate elements) -- decoded as a plain Diamond Array, since
    # Diamond has no separate Set type for this to become instead.
    count = rest.to_i()
    values = []
    index = 0
    while index < count
      values.push(redis_read_reply(conn))
      index += 1
    end
    values
  elsif reply_type == ">"
    # RESP3 Push: wire-identical to Array, semantically an unsolicited
    # out-of-band message (pub/sub messages, client-side caching
    # invalidation, ...) rather than a reply to whatever command was
    # just sent. Decoded the same way an ordinary Array is purely so a
    # reply carrying this type doesn't raise "unrecognized RESP reply
    # type" -- #command itself has no logic to recognize this as
    # unsolicited and set it aside, so if one ever arrived on an
    # ordinary RESP3 connection, it would be misread as if it were the
    # next command's own reply. See this file's own top-of-file
    # comment: nothing in this package ever triggers that in practice,
    # since SUBSCRIBE only ever runs on a dedicated RedisSubscriber
    # connection, never an ordinary one.
    count = rest.to_i()
    values = []
    index = 0
    while index < count
      values.push(redis_read_reply(conn))
      index += 1
    end
    values
  else
    raise IOError.new("unrecognized RESP reply type '#{reply_type}'")
  end
end
