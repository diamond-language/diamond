module Outer
 module Greetings
  def greet() = "hello"
 end
 class Person
  include Outer::Greetings
 end
end
Outer::Person.new().greet()
