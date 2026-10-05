# COLLECT_VARIADIC on the entry function, which is called with no arguments at
# all (a null argument pointer), naming fixed parameter 4. The result is an
# empty Array. Forming &arguments[4] from the null pointer is undefined
# behavior even though nothing is copied, so this only fails under
# make test-sanitize (UBSan), which is why it exists: the execute fuzzer found it.
b = ProgramBuilder.new()
b.emit_byte(-1, 129)
b.emit_byte(-1, 0)
b.emit_byte(-1, 0)
b.emit_byte(-1, 0)
b.emit_byte(-1, 4)
b.emit_byte(-1, 0)
b.emit_byte(-1, 0)
b.emit_byte(-1, 57)
b.emit_byte(-1, 0)
b.emit_byte(-1, 0)
b.set_register_count(-1, 1)
result = b.run()
puts(result.length())
