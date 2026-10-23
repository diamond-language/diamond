module Hidden
 private
 def answer() = 42
end
class Box
 def reveal() = self.answer()
 include Hidden
end
Box.new().reveal()
