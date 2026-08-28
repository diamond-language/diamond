def accept_value(value: Int | String) = value
def accept_items(items: Array[Int | String]) = items
def accept_keys(items: Array[String | Symbol]) = items
def accept_hash(items: Hash[String | Symbol, Int | String]) = items
def accept_nested(items: Array[Array[Int | String]]) = items
def accept_grouped(items: Hash[Symbol, Array[Int | String]]) = items
def accept_tally(items: Hash[Int | String, Int]) = items
def accept_mapped_hash(items: Hash[String | Symbol, String]) = items
def stringify(value: Int | String) -> String = "mapped"
def wrap_string(value: Int | String) -> Array[String] = ["wrapped"]
def keep(value: Int | String) -> Bool = value == value
def group(value: Int | String) -> Symbol = :all

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
puts(accept_value(arrays.first_or("fallback")))
puts(accept_value(arrays.last_or("fallback")))
puts(accept_value(hashes.fetch("answer", "fallback")))
puts(accept_items(arrays.concat(["joined"]))[1])
puts(accept_hash(hashes.merge({:joined: "joined"}))[:joined])
puts(accept_items(arrays.map(stringify))[0])
puts(accept_items(arrays.flat_map(wrap_string))[0])
puts(accept_nested(arrays.partition(keep))[0][0])
puts(accept_grouped(arrays.group_by(group))[:all][0])
puts(accept_nested(arrays.zip(["joined"]))[0][1])
puts(accept_nested(arrays.each_slice(1))[0][0])
puts(accept_nested(arrays.each_cons(1))[0][0])
puts(accept_tally(arrays.tally())[42])
puts(accept_mapped_hash(hashes.map_values(stringify))["answer"])
puts(accept_value(arrays.first_or(fallback: "fallback")))
puts(accept_value(hashes.fetch(key: "answer", fallback: "fallback")))
puts(accept_items(arrays.concat(other: ["keyword"]))[1])
puts(accept_hash(hashes.merge(other: {:keyword: "keyword"}))[:keyword])
