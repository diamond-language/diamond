b = ProgramBuilder.new()
path = "tests/multifile/bridge_cycle_a.di"
source = File.open(path, "r").read()

begin
  b.expand_source(path, source)
  1
rescue error: IOError
  42
end
