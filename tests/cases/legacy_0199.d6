module Contracts
 interface Named
  def name() -> String
 end
end
class Person
 def name() -> String = "Ada"
end
def read(value: Contracts::Named) -> String = value.name()
read(Person.new())
