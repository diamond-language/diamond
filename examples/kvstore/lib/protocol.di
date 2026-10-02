# The line protocol. Each request is one line of space-separated words;
# the last argument of SET may contain spaces. Replies, one per request:
#
#   +OK             success         :3        an integer
#   $value          a value         $         missing (nil)
#   *2 a b          a list          -ERR ...  an error
#
#   PING                    SET key value...     SETEX key seconds value...
#   GET key                 DEL key              INCR key
#   TTL key                 KEYS [prefix]        SIZE
#   QUIT
require "./store"

# A bulk reply: "$value", or a bare "$" for a missing value.
def reply_value(value) -> String
  if value == nil then "$" else "$#{value}" end
end

# Whole numbers only (an optional minus, digits).
def integer_value?(text: String) -> Bool = Regexp.new("^-?[0-9]+$").match?(text)

# Runs one request line against the store; returns the reply line, or nil
# for QUIT (the caller closes the connection).
def handle(store: Store, line: String, now: Float) -> String | Nil
  # Split into words (commands are case-insensitive) and match the command
  # plus its arguments against array patterns. A command with the wrong
  # number of arguments matches no pattern and falls to the "malformed" error
  # at the bottom.
  words = line.split(" ").reject() do |word| word.empty?() end
  command = if words.empty?() then "" else words[0].upcase() end
  args = words.drop(1)

  case [command, *args]
  when ["PING"] then "+PONG"
  when ["QUIT"] then nil
  when ["GET", key] then reply_value(store.get(key, now))
  # SET's value is everything after the key, rejoined, so it may contain
  # spaces ("SET greeting hello there"). `first` makes at least one word
  # mandatory.
  when ["SET", key, first, *rest]
    store.set(key, [first, *rest].join(" "), nil)
    "+OK"
  # SET with a time-to-live: the expiry is stored as an absolute Unix time.
  when ["SETEX", key, seconds, first, *rest]
    return "-ERR SETEX needs whole seconds" unless integer_value?(seconds) && seconds.to_i() > 0
    store.set(key, [first, *rest].join(" "), now + seconds.to_i())
    "+OK"
  when ["DEL", key] then ":#{if store.delete(key, now) then 1 else 0 end}"
  # INCR: a missing key counts as 0, and a non-numeric value is an error.
  when ["INCR", key]
    current = store.get(key, now)
    current = "0" if current == nil
    return "-ERR value is not an integer" unless integer_value?(current)
    updated = current.to_i() + 1
    # INCR keeps an existing expiry.
    ttl = store.ttl(key, now)
    store.set(key, updated.to_s(), if ttl > 0 then now + ttl else nil end)
    ":#{updated}"
  when ["TTL", key] then ":#{store.ttl(key, now)}"
  # KEYS: the reply is a count followed by the keys, sorted.
  when ["KEYS"] then "*" + [store.size(now), *store.keys("", now)].join(" ")
  when ["KEYS", prefix]
    found = store.keys(prefix, now)
    "*" + [found.length(), *found].join(" ")
  when ["SIZE"] then ":#{store.size(now)}"
  # A blank line gives an empty command.
  when [""] then "-ERR empty request"
  else "-ERR unknown or malformed command '#{command}'"
  end
end
