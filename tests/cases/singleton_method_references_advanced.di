class Tools
  def self.collect(prefix, *rest)
    prefix + ":" + rest.join(",")
  end

  def self.identity[T](value: T) -> T
    value
  end

  def self.subtract(left, right)
    left - right
  end

  def self.typed_collect[T](first: T, *rest)
    [first].concat(rest)
  end
end

module Words
  def self.collect(prefix, *rest)
    prefix + ":" + rest.join("-")
  end
end

collect = Tools.collect
identity = Tools.identity[Int]
subtract = Tools.subtract
typed_collect = Tools.typed_collect[Int]
word_collect = Words.collect

puts(collect("items", "a", "b"))
puts(collect("empty"))
puts(identity(42))
puts(subtract(right: 3, left: 10))
puts(collect(prefix: "kw", rest: "z"))
puts(typed_collect(1, 2, 3).join(","))
puts(word_collect("words", "x", "y"))
nil
