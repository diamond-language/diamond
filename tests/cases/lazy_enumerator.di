calls = 0
values = [1, 2, 3, 4].lazy().map() do |value|
  calls += 1
  value * 2
end.select() do |value|
  calls += 1
  value > 4
end

puts(calls)
puts(values.force().join(","))
puts(calls)
puts(values.to_a().join(","))
puts(calls)

range_values = (1..5).lazy().reject() do |value|
  value % 2 == 0
end.map() do |value|
  value * 10
end
puts(range_values.force().join(","))
nil
