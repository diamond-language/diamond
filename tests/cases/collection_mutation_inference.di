def accept_items(items: Array[Int | String]) = items
def accept_hash(items: Hash[String | Symbol, Int | String]) = items
def dynamic(value) = value

items = []
items.push(42)
puts(accept_items(items)[0])
items.push(value: "forty-two")
puts(accept_items(items)[1])

indexed = [0, ""]
indexed[0] = 42
indexed[1] = "indexed"
puts(accept_items(indexed)[1])

values = [40, nil]
values[0] += 2
values[1] ||= "compound"
puts(accept_items(values)[0])
puts(accept_items(values)[1])

entries = {}
entries["answer"] = 42
entries[:label] = "forty-two"
puts(accept_hash(entries)["answer"])
puts(accept_hash(entries)[:label])

opaque = [42]
opaque.push(dynamic("dynamic"))
puts(opaque[1])

opaque_entries = {"answer": 42}
opaque_entries[dynamic(:opaque)] = dynamic(true)
puts(opaque_entries[:opaque])

def captured_mutation()
  captured = []
  captured_entries = {}
  def expose_captured()
    [captured, captured_entries]
  end
  captured.push(42)
  captured.push(value: "captured")
  captured_entries["answer"] = 42
  captured_entries[:label] = "captured"
  [accept_items(captured)[1], accept_hash(captured_entries)[:label]]
end
captured_result = captured_mutation()
puts(captured_result[0])
puts(captured_result[1])
