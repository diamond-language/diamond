b = ProgramBuilder.new()
once_source = "require \"tests/multifile/math\"\n42\n"
twice_source = "require \"tests/multifile/math\"\nrequire \"tests/multifile/math\"\n42\n"
once = b.expand_source("inline.di", once_source)
twice = b.expand_source("inline.di", twice_source)
twice.length() < once.length() * 2
