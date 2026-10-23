a = [1, 2, 3, 4, 5]
s = "hello"
r = (3..)
taken = r.first(3)
visited = []
(10..).each() do |n|
  visited.push(n)
  break if n >= 12
end
grade = 75
label = case grade
        when 90.. then "A"
        when 70.. then "B"
        else "C"
        end
raised = []
begin
  r.to_a()
rescue error: RangeError
  raised.push(error.message())
end
begin
  r.length()
rescue error: RangeError
  raised.push(error.message())
end
begin
  r.last()
rescue error: RangeError
  raised.push(error.message())
end
begin
  r.step(2)
rescue error: RangeError
  raised.push(error.message())
end
[a[1..], a[2...], a[-2..], s[1..], s[5..], r.to_s(), r.endless?(), (1..4).endless?(), r.include?(2), r.include?(3), r.include?(1000), taken, visited, label, raised]
