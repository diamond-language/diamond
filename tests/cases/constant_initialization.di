def value() = LATER
begin
  value()
rescue error: RuntimeError
  puts(error.message())
end
LATER = 12
puts(value())
begin
  index = 0
  while index < 2
    ONCE = 1
    index += 1
  end
rescue error: RuntimeError
  puts(error.message())
end
