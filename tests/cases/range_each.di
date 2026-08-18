def sum_range()
  sum = 0
  def add_to_sum(x)
    sum = sum + x
  end
  (1..5).each(add_to_sum)
  sum
end
sum_range()
