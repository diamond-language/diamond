sum = 0
5.times() do |i|
  sum = sum + i
end

up = []
1.upto(5) do |i|
  up.push(i)
end

down = []
5.downto(1) do |i|
  down.push(i)
end

"#{sum}, #{up}, #{down}"
