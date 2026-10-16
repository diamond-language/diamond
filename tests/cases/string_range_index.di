s = "hello"
too_far = false
begin
  s[-6..-1]
rescue error: IndexError
  too_far = true
end
[s[1..3], s[1...3], s[1..-1], s[0...-1], s[-3..-1], s[2..1], s[5..-1], s[0..99], s[1], too_far]
