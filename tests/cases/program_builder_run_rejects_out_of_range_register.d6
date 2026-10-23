begin
  b = ProgramBuilder.new()
  # MOVE r64999, r0 -- a destination register the declared 1-register
  # frame below has no room for. Compiler-emitted bytecode can never
  # produce this (allocate_register guarantees every register it emits
  # fits the function's own register_count); this drives ProgramBuilder's
  # raw #emit_byte path directly to prove out-of-range register operands
  # get rejected before run_chunk ever sees them.
  b.emit_byte(-1, 5)
  b.emit_byte(-1, 253)
  b.emit_byte(-1, 231)
  b.emit_byte(-1, 0)
  b.emit_byte(-1, 0)
  b.emit_byte(-1, 57)
  b.emit_byte(-1, 0)
  b.emit_byte(-1, 0)
  b.set_register_count(-1, 1)
  b.run()
rescue error: RuntimeError
  42
end
