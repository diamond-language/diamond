def depth(n)
 if n <= 0
  0
 else
  depth(n - 1) + 1
 end
end
begin
 depth(1000000)
rescue error: SystemStackError
 42
end
