class Foo
 def initialize(a, b)
  @a = a
 end
end
begin
 Foo.new(1)
rescue error: ArgumentError
 42
end
