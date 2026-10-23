# Ruby's Integer#times/upto/downto return the receiver, not the block's
# own last value or an array of results -- useful for chaining, since the
# loop itself is the point.
a = 3.times() do |i|
  i * i
end
b = 2.upto(4) do |i|
  i
end
c = 4.downto(2) do |i|
  i
end
[a, b, c]
