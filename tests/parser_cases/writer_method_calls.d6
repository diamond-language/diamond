class Point
  attr_accessor x

  def initialize(x)
    @x = x
  end
end
p = Point.new(1)
puts(p.x())
p.x=(9)
puts(p.x())

module Named
  attr_reader name
  attr_writer name
end
class Person
  include Named
end
person = Person.new()
person.name=("Ada")
puts(person.name())
