parts = [1, 2, 3, 4, 5].partition() do |x|
  x - (x / 2) * 2 == 0
end
groups = [1, 2, 3, 1, 2, 1].group_by() do |x|
  x
end
"#{parts}, #{groups}"
