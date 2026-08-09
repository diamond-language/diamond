class Box
 attr_writer value
 private value=
 def assign() = self.value=(42)
end
begin
 Box.new().value=(1)
rescue error: TypeError
 Box.new().assign()
end
