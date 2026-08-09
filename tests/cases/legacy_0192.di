module Outer
 module Greetings
  def greet() = "hello"
 end
 class Person
  include Greetings
 end
end
Outer::Person.new().greet()
