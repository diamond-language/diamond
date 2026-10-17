# Array.new(n) and Array.new(n, fill) build n copies of fill (nil by default).
empty = Array.new(0)
puts(empty.length())

blank = Array.new(3)
puts(blank.length())
puts(blank[0] == nil && blank[2] == nil)

sevens = Array.new(2, 7)
puts(sevens.to_s())

n = 4
words = Array.new(n, "x")
puts(words.join(","))
words[1] = "y"
puts(words.join(","))

# Every slot holds the same fill object, as in Ruby.
shared = Array.new(2, [])
shared[0].push(1)
puts(shared.to_s())

# A length computed at run time, with the fill given as an expression.
def grid_row(width: Int) -> Array
  Array.new(width, width * 2)
end
puts(grid_row(3).to_s())

# The length must be an Int, not an Array * String join.
def sized(count)
  Array.new(count)
end
begin
  sized("a")
rescue error: TypeError
  puts(error.message())
end

begin
  sized(-1)
rescue error: ArgumentError
  puts("negative")
end
