# A class value knows its name: self.name()/self.to_s() in a class method
# (unless the class defines its own), and printing a class shows it.
class Shape
  def self.kind() = self
  def self.describe() = "#{self.name()} (#{self.to_s()})"
end
class Circle < Shape
end
class Named < Shape
  def self.name() = "custom"
end
["#{Circle.kind()}", "#{[Circle.kind(), Shape.kind()]}", Circle.describe(), Named.describe()]
