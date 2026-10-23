interface A
 def foo()
end
interface X
 def qux()
end
interface B < A, X
 def bar()
end
class C
 def foo()
  1
 end
 def qux()
  3
 end
 def bar()
  2
 end
end
C.new() is B
