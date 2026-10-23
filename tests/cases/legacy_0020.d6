class Animal
  def initialize(name: String)
    @name = name
  end

  def name() -> String = @name
end

class Dog < Animal
end

def identity(animal: Animal) -> Animal = animal

def answer(value: Int) -> Int = value + 2

identity(Dog.new("Ruby")).name() + ": " + "42"
