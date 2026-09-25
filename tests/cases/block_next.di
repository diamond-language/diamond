# `next` in a do-block ends that call of the block, with an optional value.
# Inside a loop within the block it still continues the loop.
evens = [1, 2, 3, 4].map() do |x|
  next 0 if x % 2 == 1
  x
end
kept = []
[1, 2, 3].each() do |x|
  next if x == 2
  kept.push(x)
end
count = 0
[[1, 2], [3, 4]].each() do |pair|
  pair.each() do |v|
    next if v == 3
    count += v
  end
  i = 0
  while i < 3
    i += 1
    next if i == 2
    count += 100
  end
end
[evens, kept, count]
