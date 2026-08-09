class Foo
 def bar(a, b)
  a + b
 end
end
class Baz
 def bar(a)
  a
 end
end
def call_it(x)
 x.bar(1)
end
call_it(Baz.new())
begin
 call_it(Foo.new())
rescue error: ArgumentError
 42
end
