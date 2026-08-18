sum = 0
5.times() do |i|
  sum = sum + i
end
puts(sum)

up = []
1.upto(5) do |i|
  up.push(i)
end
puts(up)

down = []
5.downto(1) do |i|
  down.push(i)
end
puts(down)

puts(3.times() do |i|
  i * i
end)
nil
