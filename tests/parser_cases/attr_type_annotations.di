class Box
 attr_accessor value: Int
end
b = Box.new()
b.value=(42)
puts(b.value())
begin
 b.value=("oops")
rescue error: TypeError
 puts("rejected")
end
nil
