begin
  b = ProgramBuilder.new()
  s = b.add_string(-1, "hello")
  b.emit_byte(-1, 1)
  b.emit_byte(-1, 0)
  b.emit_byte(-1, s)
  b.emit_byte(-1, 57)
  b.emit_byte(-1, 0)
  b.set_register_count(-1, 1)
  b.run()
rescue error: TypeError
  42
end
