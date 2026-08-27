def extract(value)
  case value
  when {"kind": "score", "payload": {"points": points, **payload_rest}, **rest}
    [points, payload_rest, rest]
  when {"kind": "heartbeat", **_}
    "heartbeat"
  else
    "other"
  end
end

event = {"kind": "score", "payload": {"points": 42, "unit": "xp"}, "source": "arena", "ranked": true}
result = extract(event)
puts(result)
result[1]["unit"] = "changed"
result[2]["source"] = "changed"
puts(event)
puts(extract({"kind": "score", "payload": {"points": 7}}))
puts(extract({"kind": "heartbeat", "sequence": 9}))
puts(extract({"other": true}))

case {"a": 1, "b": 2}
when {**everything}
  puts(everything)
end
