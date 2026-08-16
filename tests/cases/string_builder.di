sb = StringBuilder.new()
sb.append("hello")
sb.append(", ")
sb.append("world")
sb.append(42)
puts(sb.to_s())
puts(sb.length())
puts("#{sb}")

big = StringBuilder.new()
i = 0
while i < 2000
  big.append("x")
  i = i + 1
end
puts(big.length())
