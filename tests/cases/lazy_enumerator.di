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

take_calls = 0
taken = (1..100).lazy().map() do |value|
  take_calls += 1
  value * 2
end.select() do |value|
  value % 3 == 0
end.take(3)
puts(taken.join(","))
puts(take_calls)

find_calls = 0
found = [2, 4, 7, 8].lazy().find() do |value|
  find_calls += 1
  value % 2 == 1
end
puts(found)
puts(find_calls)

any_calls = 0
puts((1..100).lazy().any?() do |value|
  any_calls += 1
  value == 4
end)
puts(any_calls)

all_calls = 0
puts((1..100).lazy().all?() do |value|
  all_calls += 1
  value < 5
end)
puts(all_calls)
nil
