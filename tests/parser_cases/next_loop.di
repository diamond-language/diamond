index = 0
result = loop do
  index = index + 1
  if index < 3
    next
  end
  break index
end

puts(result)
