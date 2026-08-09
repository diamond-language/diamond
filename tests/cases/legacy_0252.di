class Box
 attr_accessor value: Int
 def raw() = @value
end
box=Box.new()
box.value=(42)
[box.value(), box.raw()]
