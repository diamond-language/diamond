def top_values() -> Array[Array[Int]] = [[10]]

class ReturnFactory
  def self.values(scale: Int = 0) -> Array[Int] = [20 + scale]
  def values(scale: Int = 0) -> Array[Int] = [30 + scale]
end

puts(top_values()[0][0] + 1)
puts(ReturnFactory.values(scale: 1)[0] + 2)
puts(ReturnFactory.new().values(scale: 2)[0] + 3)
