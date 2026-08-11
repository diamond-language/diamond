index = 0
total = 0
while index < 3
  index = index + 1
  attempts = 0
  value = begin
    attempts = attempts + 1
    raise index if index == 2 && attempts < 2
    attempts
  rescue error
    retry if attempts < 2
    error
  end
  next if index == 2
  total = total + value
end
puts(index)
puts(total)
