b = ProgramBuilder.new()
source = "puts(42)\n"
expanded = b.expand_source("inline.di", source)
expanded.length() > source.length()
