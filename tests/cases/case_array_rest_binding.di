def classify(value)
  case value
  when ["event", first, *remaining]
    [first, remaining]
  when ["empty", *_]
    "ignored"
  else
    "other"
  end
end

puts(classify(["event", 1, 2, 3]))
puts(classify(["event", 1]))
puts(classify(["empty", 4, 5]))
puts(classify(["wrong"]))
