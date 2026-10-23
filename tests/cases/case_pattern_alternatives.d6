def classify(value, minimum)
  case value
  when ["score", payload], {"score": payload} if payload > minimum
    ["score", payload]
  when ["pair", left, right], {"right": right, "left": left}
    ["pair", left, right]
  when ["event", head, *rest], {"head": head, **rest} if rest.length() > 0
    ["rest", head, rest]
  else
    "other"
  end
end

puts(classify(["score", 12], 10))
puts(classify({"score": 13}, 10))
puts(classify(["score", 9], 10))
puts(classify(["pair", 1, 2], 10))
puts(classify({"left": 3, "right": 4}, 10))
puts(classify(["event", "a", "b"], 10))
puts(classify({"head": "a", "extra": "b"}, 10))

payload = "unchanged"
case ["wrong", 99]
when ["score", payload], {"score": payload}
  nil
end
puts(payload)
