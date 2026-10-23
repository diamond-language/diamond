class Box
 private
 attr_reader value
 public
 def reveal() = self.value()
end
begin
 Box.new().value()
rescue error: TypeError
 Box.new().reveal()
end
