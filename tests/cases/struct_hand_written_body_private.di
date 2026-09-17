struct Point(x: Int, y: Int)
  private

  def helper_secret() = "shh"
end

p1 = Point.new(0, 0)
puts(p1.helper_secret())
