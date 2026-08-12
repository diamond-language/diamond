b = ProgramBuilder.new()
source = "require \"tests/multifile/math\"\ndouble(21)\n"
expanded = b.expand_source("inline.di", source)
expanded.length() > source.length()
