class Greeter
 def greet(name = "world") = name
end
[Greeter.new().greet(), Greeter.new().greet(nil)]
