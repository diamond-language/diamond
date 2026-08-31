class NoOperators
end

begin
  1 + nil
rescue error: TypeError
  puts(error.message())
end

begin
  1 - "x"
rescue error: TypeError
  puts(error.message())
end

begin
  1 * nil
rescue error: TypeError
  puts(error.message())
end

begin
  1 / nil
rescue error: TypeError
  puts(error.message())
end

begin
  1 % nil
rescue error: TypeError
  puts(error.message())
end

begin
  -"x"
rescue error: TypeError
  puts(error.message())
end

begin
  1 < "x"
rescue error: TypeError
  puts(error.message())
end

begin
  NoOperators.new() + 1
rescue error: TypeError
  puts(error.message())
end

begin
  Regexp.new("a") + 1
rescue error: TypeError
  puts(error.message())
end
