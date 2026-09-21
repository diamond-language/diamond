# JIT Phase 17: statically known Array/Hash allocation-free reads can stay
# inside compiled functions.
def array_length(values: Array) -> Int = values.length()
def hash_length(values: Hash) -> Int = values.length()
def hash_key(values: Hash) -> String = values.key_at(0)
def hash_value(values: Hash) -> Int = values.value_at(0)

i = 0
while i < 5
  puts(array_length([1, 2, 3]))
  puts(hash_length({"answer": 35}))
  puts(hash_key({"answer": 35}))
  puts(hash_value({"answer": 35}))
  i = i + 1
end
