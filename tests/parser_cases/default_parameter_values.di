def greet(name, greeting = "Hello")
  greeting + ", " + name
end
puts(greet("World"))
puts(greet("World", "Hi"))
puts(greet(greeting: "Yo", name: "Bob"))
puts(greet(name: "Ada"))

class Box
  def initialize(value = 42)
    @value = value
  end
  def show()
    @value
  end
end
puts(Box.new().show())
puts(Box.new(7).show())
