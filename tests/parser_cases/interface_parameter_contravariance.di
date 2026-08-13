class Animal
end
class Dog < Animal
end
interface Maker
  def make(value: Dog) -> Animal
end
class Good
  def make(value: Animal) -> Dog
    Dog.new()
  end
end
class BadParameter
  def make(value: String) -> Dog
    Dog.new()
  end
end

interface Consumer
  def accept(value)
end
class Typed
  def accept(value: Int)
    value
  end
end
class Dynamic
  def accept(value)
    value
  end
end

[Good.new() is Maker, BadParameter.new() is Maker,
 Typed.new() is Consumer, Dynamic.new() is Consumer]
