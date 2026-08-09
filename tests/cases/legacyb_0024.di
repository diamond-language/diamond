class Foo
 def bar(a)
  a
 end
 def self.make_patch()
  def replacement(a)
   a
  end
  replacement
 end
end
Foo.redefine_method("bar", Foo.make_patch())
