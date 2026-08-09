def run()
 def is_even(x)
  x - (x / 2) * 2 == 0
 end
 [1,3,5,6].any?(is_even)
end
run()
