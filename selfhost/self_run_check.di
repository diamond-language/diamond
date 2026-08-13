require "parser"

# Phase 4 bootstrap, the real version: does the self-hosted Parser
# (running as native-compiled bytecode) not just *compile* its own two
# source files (see self_parse_check.di), but produce a program that
# actually *runs*, and that itself correctly compiles-and-runs a third,
# independent target program? A driver snippet -- read a path, call
# parse_and_run_with_core on it, print the result -- is appended to the
# bundled parser.di+lexer.di+core.di source before compiling, so running
# the self-compiled result performs the exact same "compile and run an
# arbitrary program" operation parse_and_run_with_core.di does natively,
# except every step of it now executes as bytecode the self-hosted
# compiler produced, one VM level deeper.
def check_self_run(path)
  core_source = File.open("lib/core.di", "r").read()
  source = File.open(path, "r").read()
  builder = ProgramBuilder.new()
  expanded = builder.expand_source(path, source)
  reset = "\n#line 1\n"
  driver = "\ntarget_path = gets()\nputs(parse_and_run_with_core(target_path))\n"
  combined = core_source + reset + expanded + driver
  parser = Parser.new(combined, builder)
  parser.set_offset_correction(core_source.length() + reset.length())
  if parser.compile()
    builder.run()
  else
    puts("PARSE FAILED: " + parser.error_message())
  end
end

check_self_run(gets())
