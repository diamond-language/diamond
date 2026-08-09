begin
 begin
  1 / 0
 ensure
  40 + 2
 end
rescue error: ZeroDivisionError
 42
end
