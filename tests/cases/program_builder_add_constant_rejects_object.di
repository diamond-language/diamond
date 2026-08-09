begin
  b = ProgramBuilder.new()
  b.add_constant(-1, "hi")
rescue error: TypeError
  42
end
