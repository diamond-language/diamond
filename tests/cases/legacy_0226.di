class Box
 def assign() = self.value=(42)
 private
 def value=(incoming)
  @value = incoming
 end
end
Box.new().assign()
