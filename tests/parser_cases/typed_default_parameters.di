def greet(name: String, greeting: String = "hi")
  greeting + ", " + name
end
puts(greet("world"))
puts(greet("world", "yo"))

class Box
  def initialize(value: Int = 42)
    @value = value
  end
  def show()
    @value
  end
end
puts(Box.new().show())
puts(Box.new(7).show())
nil
