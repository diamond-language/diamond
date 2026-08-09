class Box
 attr_reader value
 private(value)
 def reveal() = self.value()
end
Box.new().reveal()
