module Helpers
 def answer() = 42
 module_function answer
end
class Box
 def reveal() = self.answer()
 include Helpers
end
Box.new().reveal()
