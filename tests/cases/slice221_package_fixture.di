b = ProgramBuilder.new()
source = "require \"roadmap_pkg\"\nroadmap_package_value()\n"
expanded = b.expand_source("inline.di", source)
expanded.length() > source.length()
