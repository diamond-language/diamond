class Box
 attr_writer value
 attr_reader value
 private value=
 def assign(v)
  self.value=(v)
 end
end
b = Box.new()
b.assign(5)
puts(b.value())
nil
