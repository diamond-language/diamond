module Hidden
 private
 def answer() = 42
end
class Box
 include Hidden
end
begin
 Box.new().answer()
rescue error: TypeError
 42
end
