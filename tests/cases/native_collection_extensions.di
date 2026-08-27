def array_second(values: Array)
  values[1]
end

def array_large(values: Array, minimum)
  values.length() >= minimum
end

def hash_invert_pairs(values: Hash)
  result = {}
  index = 0
  keys = values.keys()
  while index < keys.length()
    key = keys[index]
    result[values[key]] = key
    index += 1
  end
  result
end

puts([3, 7, 9].second())
puts([3, 7, 9].large?(3))
puts({"a": 1, "b": 2}.invert_pairs()[2])
puts({"a": 1, "b": 2}.lazy().map() do |value|
  value * 10
end.force().join(","))
nil
