class Bad
 def to_s() = 42
end
begin
 "#{Bad.new()}"
rescue error: TypeError
 42
end
