class Shape
 def initialize(width, height)
  @width = width
  @height = height
 end
 def area()
  @width * @height
 end
 def self.square_area_patch()
  def square_area()
   @width * @width
  end
  square_area
 end
end
s = Shape.new(3, 4)
before = s.area()
Shape.redefine_method("area", Shape.square_area_patch())
after = s.area()
"#{before}, #{after}"
