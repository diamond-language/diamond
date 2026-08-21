class Loud
  def to_s()
    "LOUD"
  end
end

puts([1, 2, 3].join(", "))
puts([].join(","))
puts(["a", "b", "c"].join())
puts([Loud.new(), Loud.new()].join(" | "))
