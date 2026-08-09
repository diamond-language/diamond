begin
  b = ProgramBuilder.new()
  b.nonexistent_method()
rescue error: TypeError
  42
end
