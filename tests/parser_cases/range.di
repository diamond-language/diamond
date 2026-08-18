inclusive = 1..5
exclusive = 1...5
puts(inclusive.first())
puts(inclusive.last())
puts(inclusive.exclusive?())
puts(exclusive.exclusive?())

n = 2
puts((1..n+3).last())

puts((1..5).include?(5))
puts((1...5).include?(5))
puts((1..5).length())
puts((1...5).length())
puts((5..1).length())
nil
