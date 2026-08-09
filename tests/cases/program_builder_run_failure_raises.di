begin
  b = ProgramBuilder.new()
  b.run()
rescue error: RuntimeError
  42
end
