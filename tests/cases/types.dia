class Animal
  def initialize(name: String)
    @name = name
  end

  def name() -> String
    @name
  end
end

class Dog < Animal
end

def identity(animal: Animal) -> Animal
  animal
end

def answer(value: Int) -> Int
  value + 2
end

identity(Dog.new("Ruby")).name() + ": " + "42"
