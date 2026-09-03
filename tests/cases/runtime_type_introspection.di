class Animal
end

class Dog < Animal
end

interface Named
  def name() -> String
end

class Person
  def name() -> String = "Ada"
end

dog = Dog.new()
person = Person.new()

puts([nil.class(), true.class(), 1.class(), 1.5.class(), "x".class(),
  :x.class(), [1].class(), {"x": 1}.class(), dog.class()])
puts([1.is_a?(Int), 1.is_a?(Float), dog.is_a?(Dog), dog.is_a?(Animal),
  person.is_a?(Named), "x".is_a?(Sized), nil.is_a?(Nil)])
