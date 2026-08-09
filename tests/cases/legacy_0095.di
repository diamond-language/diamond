class Foo
 def bar(a)
  a
 end
 def self.make_patch()
  def replacement(a, b)
   a + b
  end
  replacement
 end
end
begin
 Foo.redefine_method("bar", Foo.make_patch())
rescue error: ArgumentError
 42
end
