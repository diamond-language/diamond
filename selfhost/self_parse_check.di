require "parser"

# Phase 4 bootstrap check, compile-only half: can the self-hosted Parser
# (running as native-compiled bytecode) successfully compile its own two
# source files, the core prelude prepended the way parse_and_run_with_core
# does for any real target program (see core_prelude_source() in
# parser.di)? See self_run_check.di for the other half -- actually
# running the compiled result, and using it to compile and run a third,
# independent program.
def check_self_parse(path)
  core_source = core_prelude_source()
  source = File.open(path, "r").read()
  builder = ProgramBuilder.new()
  expanded = builder.expand_source(path, source)
  reset = "\n#line 1\n"
  combined = core_source + reset + expanded
  parser = Parser.new(combined, builder)
  parser.set_offset_correction(core_source.length() + reset.length())
  if parser.compile()
    puts("PARSED OK")
  else
    puts("PARSE FAILED: " + parser.error_message())
  end
end

check_self_parse(gets())
