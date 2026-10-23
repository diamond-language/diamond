def accept_ints(values: Array[Int]) = values
def accept_strings(values: Array[String]) = values
def accept_entries(values: Hash[String | Symbol, Int | String]) = values

items = []
items_alias = items
items_alias.push(42)
items_alias.push(value: "aliased")
puts(accept_ints([items[0]])[0])
puts(accept_strings([items[1]])[0])

entries = {}
entries_alias = entries
entries_alias["answer"] = 42
entries_alias[:label] = "aliased"
puts(accept_entries(entries)["answer"])
puts(accept_entries(entries)[:label])

original = []
detached = original
original = []
detached.push(42)
original.push("fresh")
puts(accept_ints(detached)[0])
puts(accept_strings(original)[0])

def captured_alias_mutation()
  items = []
  items_alias = items
  [42].each() do |value|
    items_alias.push(value)
  end
  accept_ints(items)[0]
end

puts(captured_alias_mutation())
