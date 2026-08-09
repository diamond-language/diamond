x = 0
result = loop do
  x = x + 1
  if x > 5
    break x * 2
  end
end
result
