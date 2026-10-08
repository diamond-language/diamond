# `receiver["literal"]` compiles to INDEX_GET_STRING, which looks a Hash up by the literal's
# bytes without building the key String. Every result must match the general path
# (`receiver[key_variable]`), for every receiver kind, and the forms that still need the key in
# a register (assignment, interpolation, slices) must be unaffected.
failures = []

def check(failures, label, condition)
  unless condition
    failures.push(label)
  end
end

# Hash: hit, miss, a stored nil, and String/Symbol keys with the same bytes.
h = {"a": 1, "b": nil, "key with spaces": 3, "": 4}
h[:sym] = 5
key_a = "a"
key_missing = "zz"
key_b = "b"
key_empty = ""
key_sym = "sym"
check(failures, "hit", h["a"] == 1 && h["a"] == h[key_a])
check(failures, "miss", h["zz"] == nil && h["zz"] == h[key_missing])
check(failures, "stored nil", h["b"] == nil && h.key?("b") && h["b"] == h[key_b])
check(failures, "spaces", h["key with spaces"] == 3)
check(failures, "empty key", h[""] == 4 && h[""] == h[key_empty])
check(failures, "symbol key is not a String key", h["sym"] == nil && h[key_sym] == nil && h[:sym] == 5)
check(failures, "empty hash", {}["a"] == nil)

# A Hash that has grown, had a key deleted, and was copied.
big = {}
i = 0
while i < 50
  big["k#{i}"] = i
  i += 1
end
big.delete("k10")
copy = big.dup()
check(failures, "big first", big["k0"] == 0 && copy["k0"] == 0)
check(failures, "big last", big["k49"] == 49 && copy["k49"] == 49)
check(failures, "big deleted", big["k10"] == nil && copy["k10"] == nil && copy["k11"] == 11)

# Escapes in the literal.
escaped = {"tab\there": 1, "new\nline": 2, "quote\"d": 3, "back\\slash": 4, "hash\#{x}": 5}
check(failures, "tab", escaped["tab\there"] == 1)
check(failures, "newline", escaped["new\nline"] == 2)
check(failures, "quote", escaped["quote\"d"] == 3)
check(failures, "backslash", escaped["back\\slash"] == 4)
check(failures, "escaped hash brace", escaped["hash\#{x}"] == 5)

# Interpolated keys still build the String at run time.
n = 7
numbered = {"item7": "seven"}
check(failures, "interpolated", numbered["item#{n}"] == "seven")

# Assignment forms are untouched.
counts = {}
counts["x"] = 1
counts["x"] += 4
counts["y"] ||= 9
counts["y"] ||= 10
check(failures, "assign", counts["x"] == 5 && counts["y"] == 9)

# Chained reads, and a read used inside larger expressions.
nested = {"outer": {"inner": [10, 20, 30]}}
check(failures, "chained", nested["outer"]["inner"][1] == 20)
check(failures, "in expression", (nested["outer"]["inner"][2] + 1) * 2 == 62)
check(failures, "in condition", (if nested["outer"]["missing"] == nil then "none" else "some" end) == "none")

# A typed Hash parameter carries its element type through the literal read.
def total(scores: Hash[String, Int]) -> Int
  scores["a"] + scores["b"]
end
check(failures, "typed hash", total({"a": 3, "b": 4}) == 7)

# Other receivers get the key as a real String and keep their own behavior.
class Lookup
  def initialize()
    @seen = []
  end
  def [](key)
    @seen.push(key)
    "got #{key}"
  end
  def seen() = @seen
end
lookup = Lookup.new()
check(failures, "instance override", lookup["name"] == "got name")
check(failures, "override sees a String", lookup.seen()[0] == "name" && lookup.seen()[0].length() == 4)

def read_literal(receiver)
  receiver["x"]
end

def read_variable(receiver)
  key = "x"
  receiver[key]
end

def literal_error(receiver)
  begin
    read_literal(receiver)
    "no error"
  rescue failure
    failure.message()
  end
end

def variable_error(receiver)
  begin
    read_variable(receiver)
    "no error"
  rescue failure
    failure.message()
  end
end

check(failures, "string receiver", literal_error("abc") == variable_error("abc"))
check(failures, "string receiver message", literal_error("abc") != "no error")
check(failures, "array receiver", literal_error([1, 2]) == variable_error([1, 2]))
check(failures, "array receiver message", literal_error([1, 2]) != "no error")
check(failures, "nil receiver", literal_error(nil) == variable_error(nil))
check(failures, "nil receiver message", literal_error(nil) != "no error")
check(failures, "int receiver", literal_error(5) == variable_error(5))
check(failures, "int receiver message", literal_error(5) != "no error")

# Repeated calls reach the JIT (the .env file lowers its threshold) and GC stress.
def read_many(table: Hash, lookup)
  total = 0
  index = 0
  while index < 30
    total += table["a"]
    lookup["probe"]
    index += 1
  end
  total
end
check(failures, "repeated", read_many({"a": 2}, lookup) == 60)

failures.empty?() ? "ok" : failures.join(",")
