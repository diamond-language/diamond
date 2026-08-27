class Message
  attr_reader text

  def initialize(left, middle, right = "default")
    @text = [left, middle, right].join(":")
  end

  def self.combine(left, right)
    left + right
  end

  def self.typed_pair[T](first: T, second: T)
    [first, second]
  end
end

module Words
  def self.combine(left, right)
    left + ":" + right
  end
end

puts(Message.new(right: "r", left: "l", middle: "m").text())
puts(Message.new(*["a"], middle: "b", right: "c").text())
puts(Message.combine(right: "R", left: "L"))
puts(Message.combine(*["A"], right: "B"))
puts(Message.typed_pair[Int](*[1], second: 2).join(","))
puts(Words.combine(right: "y", left: "x"))
nil
