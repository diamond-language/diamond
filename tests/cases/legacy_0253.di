class Box
 attr_accessor value: Int
 def raw() = @value
end
box=Box.new()
begin
 box.value=("wrong")
rescue error: TypeError
 box.raw()
end
