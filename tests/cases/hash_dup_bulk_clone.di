# Hash#dup copies the entry and bucket tables wholesale, so every shape of source Hash
# (empty, grown past several resizes, with deletions, mixed key kinds) must come out as an
# independent Hash that still looks up, grows, deletes and iterates correctly.
failures = []

def check(failures, label, condition)
  unless condition
    failures.push(label)
  end
end

# Empty source, then growth from nothing.
empty = {}
copy = empty.dup()
check(failures, "empty length", copy.length() == 0)
copy["a"] = 1
check(failures, "empty then insert", copy["a"] == 1 && empty.length() == 0)

# A source that has been through several resizes, with a deletion in the middle.
big = {}
i = 0
while i < 40
  big["key#{i}"] = i
  i += 1
end
big.delete("key7")
big.delete("key20")
dup = big.dup()
check(failures, "big length", dup.length() == 38)
j = 0
while j < 40
  expected = if j == 7 || j == 20 then nil else j end
  check(failures, "big lookup #{j}", dup["key#{j}"] == expected)
  j += 1
end
check(failures, "big order", dup.keys() == big.keys())
check(failures, "big equal", dup == big)

# The two are independent in both directions, including growth and deletion.
dup["extra"] = 99
big["only_original"] = 1
check(failures, "independent insert", dup["only_original"] == nil && big["extra"] == nil)
k = 0
while k < 100
  dup["more#{k}"] = k
  k += 1
end
check(failures, "grown after dup", dup.length() == 139 && dup["more99"] == 99 && dup["key39"] == 39)
dup.delete("key0")
check(failures, "delete in copy", dup["key0"] == nil && big["key0"] == 0)
check(failures, "original untouched", big.length() == 39)

# Mixed key kinds and shared (not deep-copied) values.
list = [1, 2]
mixed = {1: "int", "s": "str", true: "bool", nil: "nil", 2.5: "float", "arr": list}
mixed_copy = mixed.dup()
check(failures, "mixed int", mixed_copy[1] == "int")
check(failures, "mixed string", mixed_copy["s"] == "str")
check(failures, "mixed bool", mixed_copy[true] == "bool")
check(failures, "mixed nil", mixed_copy[nil] == "nil")
check(failures, "mixed float", mixed_copy[2.5] == "float")
list.push(3)
check(failures, "values are shared", mixed_copy["arr"].length() == 3)

# Copying a copy.
again = dup.dup().dup()
check(failures, "copy of copy", again == dup)

failures.empty?() ? "ok" : failures.join(",")
