def abs_value(x) = if x < 0
  -x
else
  x
end
puts([1, -5, 3, -2].min_by() do |x| abs_value(x) end)
puts([1, -5, 3, -2].max_by() do |x| abs_value(x) end)
puts([1, 2, 3, 4, 5].take(3))
puts([1, 2, 3, 4, 5].drop(3))
puts([1, 2, 3].flat_map() do |x| [x, x * 10] end)
puts([1, 2, 3, 4, 5].partition() do |x| x - (x / 2) * 2 == 0 end)
puts([1, 2, 3, 1, 2, 1].group_by() do |x| x end)
puts([1, 2, 3].zip([4, 5]))
puts([1, 2, 3, 4, 5].each_slice(2))
puts([1, 2, 3, 4, 5].each_cons(2))
puts([1, 2, 2, 3, 3, 3].tally())
nil
