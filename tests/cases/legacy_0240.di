module Helpers
 module_function
 def answer() = 42
end
class Box
 def reveal() = self.answer()
 include Helpers
end
begin
 Box.new().answer()
rescue error: TypeError
 Box.new().reveal()
end
