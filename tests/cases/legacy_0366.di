def run()
 def anything(x)
  x
 end
 def add(acc, x)
  acc + x
 end
 a = enumerable_select([], anything)
 b = enumerable_count([], anything)
 c = enumerable_map([], anything)
 d = enumerable_reduce([], 0, add)
 "#{a}, #{b}, #{c}, #{d}"
end
run()
