def same_left(value: Int) -> Int = value
def same_right(value: Int) -> Int = value + 1
def different(value: Int) -> String = "dynamic"

agreed = if ARGV.length() == 0
  same_left
else
  same_right
end
puts(agreed(40) + 2)

divergent = if ARGV.length() == 0
  same_left
else
  different
end
puts(divergent(39) + 3)
