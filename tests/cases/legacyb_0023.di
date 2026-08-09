begin
 begin
  1/0
 rescue error: ZeroDivisionError
  raise
 end
rescue outer: ZeroDivisionError
 outer.message()
end
