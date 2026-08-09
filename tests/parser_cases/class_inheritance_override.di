class Animal
  def initialize(name)
    @name = name
  end
  def name()
    @name
  end
  def speak()
    0
  end
end
class Dog < Animal
  def speak()
    1
  end
end
Dog.new("Rex").speak()
