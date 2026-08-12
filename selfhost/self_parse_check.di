require "parser"

# Phase 4 bootstrap check: can the self-hosted Parser (running as
# native-compiled bytecode) successfully compile its own two source
# files, lib/core.di prepended the way parse_and_run_with_core does for
# any real target program? Doesn't yet run the result -- see
# docs/roadmap.md's self-hosting Phase 3 follow-up entries for how far
# that is from here.
def check_self_parse(path)
  core_source = File.open("lib/core.di", "r").read()
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
