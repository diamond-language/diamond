def abs_value(x) = if x < 0
  -x
else
  x
end
low = [1, -5, 3, -2].min_by() do |x|
  abs_value(x)
end
high = [1, -5, 3, -2].max_by() do |x|
  abs_value(x)
end
"#{low}, #{high}"
