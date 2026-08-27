expected_kind = "score"
expected_points = 42

def classify(value, expected_kind, expected_points)
  case value
  when [^expected_kind, [^expected_points, label], *tail]
    ["matched", label, tail]
  else
    "other"
  end
end

puts(classify(["score", [42, "Ada"], 7, 8], expected_kind, expected_points))
puts(classify(["other", [42, "Ada"]], expected_kind, expected_points))
puts(classify(["score", [41, "Ada"]], expected_kind, expected_points))

value = "outer"
case ["wrong", "replacement"]
when [^expected_kind, value]
  nil
end
puts(value)

def make_matcher(expected)
  def matcher(candidate)
    case candidate
    when [^expected]
      "captured"
    else
      "miss"
    end
  end
  matcher
end

matcher = make_matcher("signal")
puts(matcher(["signal"]))
puts(matcher(["noise"]))
