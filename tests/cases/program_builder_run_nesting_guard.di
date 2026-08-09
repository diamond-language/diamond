def deep(n)
  if n == 0
    b = ProgramBuilder.new()
    b.run()
    0
  else
    deep(n - 1)
  end
end
begin
  deep(20)
rescue error: SystemStackError
  42
end
