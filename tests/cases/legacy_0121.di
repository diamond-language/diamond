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
class BadReturn
 def make(value: Animal) -> String
  "no"
 end
end
[Good.new() is Maker, BadParameter.new() is Maker, BadReturn.new() is Maker]
