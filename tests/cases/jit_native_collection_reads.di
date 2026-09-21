# JIT Phase 17: statically known Array/Hash allocation-free reads can stay
# inside compiled functions.
def array_length(values: Array) -> Int = values.length()
def hash_length(values: Hash) -> Int = values.length()
def hash_key(values: Hash) -> String = values.key_at(0)
def hash_value(values: Hash) -> Int = values.value_at(0)
def string_length(value: String) -> Int = value.length()
def string_index(value: String, needle: String) = value.index_of(needle)
def string_ord(value: String) -> Int = value.ord()
def string_slice(value: String, start: Int, length: Int) -> String = value.slice(start, length)
def string_slice_untyped(value: String, start, length) = value.slice(start, length)

i = 0
while i < 5
  puts(array_length([1, 2, 3]))
  puts(hash_length({"answer": 35}))
  puts(hash_key({"answer": 35}))
  puts(hash_value({"answer": 35}))
  puts(string_length("pikachu"))
  puts(string_index("pikachu", "achu"))
  puts(string_ord("A"))
  puts(string_slice("pikachu", 3, 99))
  string_slice_untyped("pikachu", 0, 4)
  i = i + 1
end
begin
  string_slice("x", 2, 1)
rescue error: IndexError
  puts("slice bounds")
end
begin
  string_slice_untyped("x", "bad", 1)
rescue error: TypeError
  puts("slice types")
end
puts(string_index("pikachu", "z"))
begin
  string_ord("")
rescue error: IndexError
  puts("empty ord")
end
