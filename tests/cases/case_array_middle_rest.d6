def classify(value)
  case value
  when ["event", first, *middle, last] if middle.length() >= 1
    [first, middle, last]
  when ["pair", left, *_, right]
    [left, right]
  else
    "other"
  end
end

puts(classify(["event", 1, 2, 3, 4]))
puts(classify(["event", 1, 4]))
puts(classify(["pair", 5, 6]))
puts(classify(["pair", 5, 7, 8, 6]))
puts(classify(["short"]))

case ["outer", [1, 2, 3, 4], "done"]
when ["outer", [left, *middle, right], tail]
  puts([left, middle, right, tail])
end
