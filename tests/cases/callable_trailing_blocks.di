def transform(value, &block)
  yield(value)
end

callback = transform
offset = 2
puts(callback(5) do |value|
  value + offset
end)

puts(callback(*[6]) do |value|
  value * offset
end)
