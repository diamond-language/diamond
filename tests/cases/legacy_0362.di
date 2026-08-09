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
 bag = NumberBag.new([1,2,3,4,5,6])
 bag.select(is_even)
end
run()
