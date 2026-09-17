# struct-generated readers (docs/classes-and-modules.md's struct
# declarations section) at construction + field-read throughput --
# compare this file's own per-iteration time against
# hash_ivar_construct.di/object_hydration.di's hand-written-class
# equivalents to confirm struct's generated initialize/readers compile
# to the same shape as a hand-written class, not a slower generic path.
# No prior bench/*.di coverage existed for struct at all.
struct Point(x: Int, y: Int)
end

def run()
  total = 0
  index = 0
  while index < 200000
    p = Point.new(index, index + 1)
    total = total + p.x() + p.y()
    index = index + 1
  end
  total
end
run()
