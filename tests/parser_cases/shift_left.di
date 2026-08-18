puts(5 << 2)
puts(1 + 2 << 3)

arr = [1, 2]
arr << 3 << 4
puts(arr)

begin
  1 << 64
rescue error: RangeError
  puts("range error caught")
end

begin
  "str" << 1
rescue error: TypeError
  puts("type error caught")
end
nil
