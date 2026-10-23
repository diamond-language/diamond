expected_kind = "score"

def classify(value, expected_kind)
  case value
  when {"kind": ^expected_kind, "payload": {"points": points, "tags": [first, *rest]}}
    ["matched", points, first, rest]
  when {"kind": "heartbeat", "payload": _}
    "heartbeat"
  else
    "other"
  end
end

puts(classify({"kind": "score", "payload": {"points": 42, "tags": ["fast", "ranked"]}, "extra": true}, expected_kind))
puts(classify({"kind": "score", "payload": {"points": 42, "tags": ["solo"]}}, expected_kind))
puts(classify({"kind": "heartbeat", "payload": nil}, expected_kind))
puts(classify({"kind": "score", "payload": {"points": 42}}, expected_kind))
puts(classify({"kind": "other", "payload": {}}, expected_kind))
puts(classify(["not", "hash"], expected_kind))

points = "unchanged"
case {"kind": "score", "payload": {"points": 9, "tags": []}}
when {"kind": ^expected_kind, "payload": {"points": points, "tags": [_, *rest]}}
  nil
end
puts(points)

case {}
when {}
  puts("empty hash")
end
