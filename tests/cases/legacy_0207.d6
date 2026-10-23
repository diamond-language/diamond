module Tools
 def self.answer() = 42
 def included() = 1
end
class Box
 include Tools
end
begin
 Box.new().answer()
rescue error: NoMethodError
 Tools.answer()
end
