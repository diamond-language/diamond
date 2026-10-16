a = [1, 2, 3, 4]
too_far = false
begin
  a[-5..-1]
rescue error: IndexError
  too_far = true
end
[a[1..-1], a[-2..-1], a[0...-1], a[0..-2], a[-3...-1], a[2..-4], a[4..-1], a[0..10], too_far]
