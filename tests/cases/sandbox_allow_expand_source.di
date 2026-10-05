# The filesystem allow-list category turns expand_source back on.
text = ProgramBuilder.new().expand_source("main.di", "1 + 1\n")
text.length() > 0
