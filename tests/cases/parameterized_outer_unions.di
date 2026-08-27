def accept(values: Array[Int] | Array[String]) -> Int
  values.length()
end

def accept_nested(values: Array[Array[Int] | Array[String]]) -> Int
  values.length()
end

puts(accept([1, 2]))
puts(accept(["one"]))
puts(accept_nested([[1], ["two"]]))

condition = ARGV.length() == 0
selected = if condition
  [1]
else
  ["one"]
end
puts(accept(selected))
