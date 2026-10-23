def boom()
  raise "short circuit failed"
end

def grade(score, override)
  case
  when score >= 90
    "excellent"
  when score >= 70, override
    "passing"
  else
    "retry"
  end
end

puts(grade(95, false))
puts(grade(75, false))
puts(grade(50, true))
puts(grade(50, false))

enabled = true
puts(case
when true, boom() if enabled
  "short circuited"
else
  "bad"
end)

puts(case
when false if boom()
  "bad"
when []
  "array literal truthy"
else
  "miss"
end)

puts(case
when nil, false
  "bad"
when {"ok": true}
  "hash literal truthy"
end)
