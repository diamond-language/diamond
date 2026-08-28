def accept_value(value: Int | String) = value
def accept_items(items: Array[Int | String]) = items
def accept_keys(items: Array[String | Symbol]) = items

arrays = if ARGV.length() == 0
  [42]
else
  ["forty-two"]
end

hashes = if ARGV.length() == 0
  {"answer": 42}
else
  {:answer: "forty-two"}
end

puts(accept_value(arrays.first()))
puts(accept_value(arrays.last()))
puts(accept_items(arrays.reverse())[0])
puts(accept_items(arrays.uniq())[0])
puts(accept_items(arrays.compact())[0])
puts(accept_items(arrays.sort())[0])
puts(accept_items(arrays.sort_by() do |value|
  value.to_s()
end)[0])
puts(accept_items(arrays.select() do |value|
  value == value
end)[0])
puts(accept_items(arrays.reject() do |value|
  value != value
end)[0])
puts(accept_items(arrays.take(1))[0])
puts(accept_items(arrays.drop(0))[0])
puts(accept_keys(hashes.keys())[0])
puts(accept_items(hashes.values())[0])
