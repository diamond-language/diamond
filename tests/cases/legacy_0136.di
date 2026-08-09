interface Greeter
 def greet(name: String) -> String
end
class Friendly
 def greet(name: String = "world") -> String = name
end
Friendly.new() is Greeter
