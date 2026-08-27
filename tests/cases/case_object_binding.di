class Event
  attr_reader kind, payload

  def initialize(kind, payload)
    @kind = kind
    @payload = payload
  end
end

class ScoredEvent < Event
  attr_reader source

  def initialize(kind, payload, source)
    super(kind, payload)
    @source = source
  end
end

expected_kind = "score"

def classify(value, expected_kind)
  case value
  when ScoredEvent{kind: ^expected_kind, payload: {"points": points, "tags": [first, *rest]}, source: source}
    ["scored", points, first, rest, source]
  when Event{kind: "heartbeat", payload: _}
    "heartbeat"
  else
    "other"
  end
end

puts(classify(ScoredEvent.new("score", {"points": 42, "tags": ["fast", "ranked"]}, "arena"), expected_kind))
puts(classify(Event.new("heartbeat", nil), expected_kind))
puts(classify(Event.new("score", {"points": 1, "tags": ["solo"]}), expected_kind))
puts(classify({"kind": "score"}, expected_kind))

points = "unchanged"
case ScoredEvent.new("score", {"points": 9, "tags": []}, "arena")
when ScoredEvent{kind: ^expected_kind, payload: {"points": points, "tags": [_, *rest]}, source: _}
  nil
end
puts(points)

case ScoredEvent.new("anything", nil, "arena")
when Event{}
  puts("subclass")
end
