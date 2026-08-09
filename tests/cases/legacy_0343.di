def run()
 def is_even(x)
  x - (x / 2) * 2 == 0
 end
 enumerable_any([1,3,5], is_even)
end
run()
