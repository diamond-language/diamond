# Run by smoke_test.sh: a loaded index and a parsed Version are frozen, so
# changing one raises FrozenError instead of silently succeeding.
require "../lib/index"

index = load_index("testdata/index.txt")
begin
  index.add(Release.new("x", Version.parse("1.0.0"), []))
  puts("index accepted a write")
rescue error: FrozenError
  puts("index frozen: #{error.message()}")
end

version = Version.parse("1.2.3")
puts("version frozen: #{version.frozen?()}")

# `unique` is generic: its Array[T] argument is checked at the call.
begin
  unique(5)
rescue error: TypeError
  puts("unique(5): #{error.message()}")
end

exit(0)
