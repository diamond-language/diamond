def classify(n)
  case n
  when 0
    "zero"
  when 1, 2, 3
    "small"
  else
    "big"
  end
end
puts(classify(0))
puts(classify(2))
puts(classify(100))

puts(case 5
when 5 then "five"
else "other"
end)

def boom()
  raise "should not be called"
end
x = 1
puts(case x
when 1, boom()
  "matched first"
end)

z = 10
r = case z
when 10
  z = 99
  z
else
  0
end
puts(r)
puts(z)
nil
