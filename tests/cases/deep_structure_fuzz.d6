# A deterministic fuzzer for very deep and self-containing data. It builds
# chains of nested Arrays, Hashes and Box instances at depths chosen around
# every bound the runtime has (91 for JSON, 256, 4096 for cross-thread copies,
# then far beyond), some of them closed into a cycle, and pushes each through
# the operations that walk a structure: to_s, ==, hashing, dup, freeze,
# deep_freeze, JSON.stringify, flatten, sort, uniq, and a Thread/Channel round
# trip. The process must survive all of it: each operation may succeed or
# raise a normal error, but never crash. The outcome counts are printed so a
# change in which operations succeed shows up as a diff.
#
# Properties checked on the acyclic Array/Hash chains, which must hold at any
# depth that an operation succeeds at:
#   - a value equals a freshly built copy of itself, and differs from one whose
#     deepest leaf is different
#   - a value used as a Hash key is found again by an equal copy
#   - a Thread/Channel round trip, when it succeeds, yields an equal value
# and two separately built cycles of the same shape compare equal.
class Box
  def initialize(inner)
    @inner = inner
  end

  def inner() = @inner

  def set(inner)
    @inner = inner
  end
end

class Rng
  def initialize(seed)
    @state = seed
  end

  def draw(limit)
    @state = (@state * 1103515245 + 12345) % 2147483648
    (@state / 65536) % limit
  end
end

# Wraps `inner` in one more level of the given kind.
def wrap(kind, inner)
  if kind == 0
    [inner]
  elsif kind == 1
    {"k": inner}
  else
    Box.new(inner)
  end
end

# Builds a chain `depth` levels deep around `leaf`. `mixed` allows Box levels;
# otherwise only Arrays and Hashes are used. Kinds are drawn from `seed`, so the
# same arguments always build an equal structure.
def build(seed, depth, mixed, leaf)
  rng = Rng.new(seed)
  value = leaf
  depth.times() do |level|
    kind = rng.draw(mixed ? 3 : 2)
    value = wrap(kind, value)
  end
  value
end

# Builds the chain around a mutable bottom container, then points the bottom
# back at the top so the structure contains itself.
def build_cycle(seed, depth, kind)
  bottom = kind == 0 ? [] : (kind == 1 ? {} : Box.new(nil))
  top = build(seed, depth, kind == 2, bottom)
  if kind == 0
    bottom.push(top)
  elsif kind == 1
    bottom["back"] = top
  else
    bottom.set(top)
  end
  top
end

OPS = ["to_s", "equal", "hash_key", "dup", "freeze", "deep_freeze", "json",
       "flatten", "sort", "uniq", "thread", "channel"]

def run_op(name, value)
  if name == "to_s"
    value.to_s().length()
  elsif name == "equal"
    value == value.dup()
  elsif name == "hash_key"
    lookup = {}
    lookup[value] = 1
    lookup[value]
  elsif name == "dup"
    value.dup()
  elsif name == "freeze"
    value.freeze()
  elsif name == "deep_freeze"
    value.deep_freeze()
  elsif name == "json"
    JSON.stringify(value)
  elsif name == "flatten"
    value.flatten()
  elsif name == "sort"
    [value, value].sort()
  elsif name == "uniq"
    [value, value].uniq()
  elsif name == "thread"
    Thread.new(identity, value).join()
  else
    channel = Channel.new(1)
    channel.send(value)
    channel.receive()
  end
end

def identity(value) = value

# A shape's name for the outcome table: depth bucket and kind.
DEPTHS = [0, 1, 2, 50, 90, 91, 92, 255, 256, 257, 1000, 4095, 4096, 4097, 20000, 50000]

outcomes = {}
failures = []

def record(outcomes, key)
  outcomes[key] = outcomes.fetch(key, 0) + 1
end

# Acyclic chains: every kind mix at every depth, several seeds.
seeds = [11, 22]
seeds.each() do |seed|
  DEPTHS.each() do |depth|
    [false, true].each() do |mixed|
      value = build(seed, depth, mixed, 1)
      OPS.each() do |op|
        begin
          run_op(op, value)
          record(outcomes, "acyclic #{op}: ok")
        rescue e
          record(outcomes, "acyclic #{op}: #{e.class()}")
        end
      end

      next if mixed
      same = build(seed, depth, false, 1)
      other = build(seed, depth, false, 2)
      failures.push("#{seed}/#{depth}: not equal to its rebuild") unless value == same
      failures.push("#{seed}/#{depth}: equal to a different leaf") if value == other
      lookup = {}
      lookup[value] = "found"
      failures.push("#{seed}/#{depth}: hash key lookup missed") unless lookup[same] == "found"
      begin
        channel = Channel.new(1)
        channel.send(value)
        copy = channel.receive()
        failures.push("#{seed}/#{depth}: channel copy differs") unless copy == value
      rescue e
        nil
      end
    end
  end
end

# Self-containing structures, closed after building: nothing may crash or hang.
[0, 1, 2].each() do |kind|
  [1, 2, 50, 300, 5000].each() do |depth|
    value = build_cycle(7 + depth, depth, kind)
    if kind < 2
      again = build_cycle(7 + depth, depth, kind)
      failures.push("cycle #{kind}/#{depth}: equal shapes compare unequal") unless value == again
    end
    OPS.each() do |op|
      begin
        run_op(op, value)
        record(outcomes, "cyclic #{op}: ok")
      rescue e
        record(outcomes, "cyclic #{op}: #{e.class()}")
      end
    end
  end
end

outcomes.keys().sort().each() do |key|
  puts("#{key} x#{outcomes[key]}")
end
puts("property failures: #{failures.length()}")
failures.each() do |failure|
  puts(failure)
end
