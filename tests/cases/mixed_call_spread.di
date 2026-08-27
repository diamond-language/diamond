def merge_values(*values)
  values.join("-")
end

middle = [2, 3]
puts(merge_values(1, *middle, 4))

callable = merge_values
puts(callable("a", *["b", "c"], "d"))

class Receiver
  def combine(*values) = values.join(":")
end
puts(Receiver.new().combine("a", *["b", "c"], "d"))

class Constructed
  attr_reader values
  def initialize(*values)
    @values = values
  end
end
puts(Constructed.new(1, *[2, 3], 4).values().join(","))

module Singleton
  def self.combine(*values) = values.join("/")
end
puts(Singleton.combine("a", *["b", "c"], "d"))
nil
