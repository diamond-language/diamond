def run()
 def is_even(x)
  x - (x / 2) * 2 == 0
 end
 enumerable_select([1,2,3,4,5,6], is_even)
end
run()
