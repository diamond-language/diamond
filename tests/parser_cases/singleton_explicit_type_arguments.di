class Box
 def self.identity[T](value: T) -> T = value
end
puts(Box.identity[String]("hi"))

module Named
 def self.identity[T](value: T) -> T = value
end
puts(Named.identity[Int](7))
nil
