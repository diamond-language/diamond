interface Greetable
 def greet(name)
end
class Person
 def greet(name) -> String
  name
 end
end
def greet(value: Greetable) -> String
 value.greet("hi")
end
[greet(Person.new()), Person.new() is Greetable]
