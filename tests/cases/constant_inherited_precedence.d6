# Lookup order: the class's own constant, an enclosing namespace's, then
# ancestors', then the top level.
Z = "top"
class Base
  Z = "base"
  W = "base-w"
end
class Child < Base
  def z() = Z
end
class Own < Base
  Z = "own"
  def z() = Z
end
module Outer
  W = "outer-w"
  class Inner < Base
    def w() = W
  end
end
puts(Child.new().z())
puts(Own.new().z())
puts(Base::Z)
puts(Outer::Inner.new().w())
