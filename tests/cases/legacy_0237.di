module Helpers
 def answer() = 42
 module_function answer
end
class Box
 include Helpers
end
begin
 Box.new().answer()
rescue error: TypeError
 Helpers.answer()
end
