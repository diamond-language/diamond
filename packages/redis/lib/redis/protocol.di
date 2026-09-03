# RESP2 (REdis Serialization Protocol, version 2) wire encode/decode --
# the one thing every Redis command shares, regardless of which data
# type it operates on. Deliberately RESP2 only, not RESP3: every real
# Redis server still speaks it (RESP3 needs an explicit `HELLO 3`
# opt-in), it's simpler (fewer reply types), and nothing this package
# covers needs RESP3's extra ones (doubles, booleans, maps, sets, big
# numbers, out-of-band push messages).
#
# A command is always sent the same way regardless of which one it is
# -- a RESP Array of Bulk Strings (the "multibulk" form real clients
# use, as opposed to Redis's older inline-command wire format, which
# this package never emits): `SET foo bar` becomes
# `*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n`. A reply's *type*
# depends on the command and can be any of the five RESP2 kinds below,
# decoded generically here -- redis_read_reply doesn't know or care
# which command produced what it's reading.

# One error class for anything Redis's own server reports back as an
# error reply (`-ERR ...`, `-WRONGTYPE ...`, etc.) -- matching
# SQLite3Error's own "one class per external system" precedent
# (docs/databases.md) rather than trying to model Redis's own informal
# error-code-prefix convention as distinct Diamond exception types.
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

# Reads and decodes exactly one RESP2 reply -- recursively, for the
# Array type, since an array's own elements are themselves complete
# RESP2 values (including further arrays, e.g. a transaction's own
# EXEC reply: one array of each queued command's own reply).
#
# `nil` is RESP2's own encoding for "no value" in two shapes: a null
# bulk string (`$-1\r\n`, e.g. GET on a missing key) and a null array
# (`*-1\r\n`, e.g. BLPOP timing out) -- both collapse to the same
# Diamond `nil`, matching how callers already treat "not found" as nil
# everywhere else in this ecosystem (ActiveRecord's own #find, Hash
# lookups, ...).
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
      data = redis_read_exact(conn, length)
      conn.gets() # the bulk string's own trailing CRLF
      data
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
  else
    raise IOError.new("unrecognized RESP reply type '#{reply_type}'")
  end
end
