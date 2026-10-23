begin
  b = ProgramBuilder.new()
  # CHECK_DESTRUCTURE_COUNT r0, 0 -- r0 is nil (registers start nil),
  # never an Array. Ordinary compiled `a, b = expr` always emits a
  # CHECK_TYPE guard immediately before this opcode (compile_multi_
  # assignment, src/compiler.c), so this opcode's own handler used to
  # trust that pairing unconditionally and dereference the register's
  # object pointer with no check at all -- a real type-confusion SEGV
  # for hand-assembled bytecode that skips the guard, found by
  # fuzz/execute_fuzzer.c. This drives ProgramBuilder's raw #emit_byte
  # path directly to prove a non-Array value is rejected cleanly
  # instead of crashing.
  b.emit_byte(-1, 94)
  b.emit_byte(-1, 0)
  b.emit_byte(-1, 0)
  b.emit_byte(-1, 0)
  b.emit_byte(-1, 0)
  b.set_register_count(-1, 1)
  b.run()
rescue error: RuntimeError
  42
end
