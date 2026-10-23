attempts = 0
value = begin
  attempts = attempts + 1
  raise attempts if attempts < 3
  attempts
rescue error
  retry if attempts < 3
  error
end
puts(value)
puts(attempts)
