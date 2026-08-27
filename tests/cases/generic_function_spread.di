def identity[T](value: T) -> T
  value
end

puts(identity[Int](*[42]))
puts(identity[String]("prefix", *[]) )

class GenericReceiver
  def identity[T](value: T) -> T
    value
  end
end
puts(GenericReceiver.new().identity[String](*["method"]))

module GenericSingleton
  def self.identity[T](value: T) -> T
    value
  end
end
puts(GenericSingleton.identity[Int](*[9]))
nil
