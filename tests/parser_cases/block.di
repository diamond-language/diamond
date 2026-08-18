def apply(x, callback)
  callback(x)
end
puts(apply(5) do |n|
  n * 2
end)

total = 0
[1, 2, 3, 4].each() do |x|
  total = total + x
end
puts(total)

puts([1, 2, 3, 4, 5, 6].select() do |x|
  x - (x / 2) * 2 == 0
end)

puts([1, 2, 3].reduce(0) do |acc, x|
  acc + x
end)

def run_with_receiver_capture()
  n = 100
  [1, 2, 3].reduce(n) do |acc, x|
    acc + x
  end
end
puts(run_with_receiver_capture())

nested = [1, 2, 3].map() do |x|
  [10, 20].reduce(0) do |acc, y|
    acc + x * y
  end
end
puts(nested)

def call_it(callback)
  callback()
end
zero_param = call_it() do
  99
end
puts(zero_param)
nil
