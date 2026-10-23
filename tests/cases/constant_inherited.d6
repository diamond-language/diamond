# A subclass reads its superclass's (and grandparent's) constants,
# unqualified from any method and qualified from anywhere.
class Shape
  SIDES = 0
  LABEL = "shape"
  def label() = LABEL
end
class Polygon < Shape
  SIDES = 3
  def sides() = SIDES
end
class Triangle < Polygon
  def describe() = LABEL + ":" + SIDES.to_s()
  def self.label_of() = LABEL
end
puts(Triangle.new().describe())
puts(Triangle.new().label())
puts(Triangle.label_of())
puts(Triangle::SIDES)
puts(Triangle::LABEL)
puts(Polygon::LABEL)
puts(Shape::SIDES)
