require "parser"

def check(path)
  source = File.open(path, "r").read()
  builder = ProgramBuilder.new()
  source = builder.expand_source(path, source)
  parser = Parser.new(source, builder)
  if parser.compile()
    raise RuntimeError.new("expected parser failure")
  else
    puts(parser.error_message())
    raise RuntimeError.new("expected parser failure")
  end
end

check(gets())
