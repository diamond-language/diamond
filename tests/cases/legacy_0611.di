interface Adder
 def add(a,
  b)
end
class C
 def add(a, b)
  a + b
 end
end
C.new() is Adder
