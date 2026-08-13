begin
  b = ProgramBuilder.new()
  fn = b.declare_function("inner", 0, 0)
  b.emit_byte(fn, 57)
  b.emit_byte(fn, 0)
  b.set_register_count(fn, 1)

  b.emit_byte(-1, 31)
  b.emit_byte(-1, 0)
  b.emit_byte(-1, 0)
  b.emit_byte(-1, fn)
  b.emit_byte(-1, 0)
  b.emit_byte(-1, 57)
  b.emit_byte(-1, 0)
  b.set_register_count(-1, 1)
  b.run()
rescue error: TypeError
  42
end
