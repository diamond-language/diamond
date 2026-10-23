module Mixed
 attr_reader first, second
 private first, second
end
class Box
 include Mixed
 def reveal() = [self.first(), self.second()]
end
Box.new().reveal()
