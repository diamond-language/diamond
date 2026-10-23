def explode()
  raise "guard should not run"
end

def classify(value, minimum)
  case value
  when {"score": score, **metadata} if score > minimum && metadata.length() > 0
    ["accepted", score, metadata]
  when {"score": score} if score == minimum
    ["exact", score]
  else
    "other"
  end
end

puts(classify({"score": 12, "source": "arena"}, 10))
puts(classify({"score": 10}, 10))
puts(classify({"score": 9, "source": "arena"}, 10))

score = "unchanged"
result = case {"score": 5}
when {"score": score} if score > 10
  "first"
when {"score": fallback} if fallback == 5
  "second"
else
  "none"
end
puts([result, score, fallback])

puts(case {"other": 1}
when {"score": score} if explode()
  "bad"
else
  "structural miss"
end)

enabled = true
puts(case 5
when 5 if enabled
  "scalar guard"
else
  "miss"
end)
