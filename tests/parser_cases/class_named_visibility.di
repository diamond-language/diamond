class Box
 def initialize(v)
  @v = v
 end
 def peek()
  @v
 end
 def reveal()
  self.peek()
 end
 private peek
end
b = Box.new(5)
puts(b.reveal())
nil
