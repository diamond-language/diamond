class NumberBag
 include Enumerable
 def initialize(values)
  @values = values
 end
 def each(callback)
  @values.each(callback)
 end
end
def run()
 def is_even(x)
  x - (x / 2) * 2 == 0
 end
 def double(x)
  x * 2
 end
 def add(acc, x)
  acc + x
 end
 bag = NumberBag.new([1,2,3,4])
 a = bag.count(is_even)
 b = bag.any?(is_even)
 c = bag.all?(is_even)
 d = bag.map(double)
 e = bag.reduce(0, add)
 "#{a}, #{b}, #{c}, #{d}, #{e}"
end
run()
