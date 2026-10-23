struct Point(x: Int, y: Int)
end

p = Point.new(1, 2)
puts(p.frozen?())
p.freeze()
puts(p.frozen?())
begin
  p.freeze()
  puts("still fine")
rescue FrozenError
  puts("unreachable")
end
