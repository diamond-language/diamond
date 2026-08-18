total = 0
[1, 2, 3, 4].each() do |x|
  total = total + x
end
evens = [1, 2, 3, 4, 5, 6].select() do |x|
  x - (x / 2) * 2 == 0
end
doubled = [1, 2, 3].map() do |x|
  x * 2
end
summed = [1, 2, 3, 4].reduce(0) do |acc, x|
  acc + x
end
"#{total}, #{evens}, #{doubled}, #{summed}"
