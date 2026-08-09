class Bad
 def +()
  42
 end
end
begin
 Bad.new() + 5
rescue error: ArgumentError
 99
end
