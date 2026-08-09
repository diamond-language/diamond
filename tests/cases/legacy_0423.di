def run()
 def cb(x) = true
 def add(acc,x) = acc+x
 s = [].select(cb)
 c = [].count(cb)
 m = [].map(cb)
 r = [].reduce(99, add)
 "#{s}, #{c}, #{m}, #{r}"
end
run()
