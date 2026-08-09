module Greetable
 def greet(name: String) -> String = "Hello, #{name}"
end
class Person
 include Greetable
end
Person.new().greet("sir")
