# Array#-, Array#& and Array#combination are linear in their input/output (the
# first two used to rescan the other array per element, which took minutes at a
# few tens of thousands of elements), and keep the semantics: scalars match by
# ==, so 2.0 is removed by 2, and a user-defined == on instances is still called.
class P
  def initialize(x)
    @x = x
  end
  def ==(other)
    other is P && @x == other.x()
  end
  def x() = @x
end
puts(([1, 2, 3, 2, "a", :b, nil, 2.0] - [2, "a", nil]).to_s())
puts(([1, 2, 3, 2, "a"] & [2, "a", 9]).to_s())
puts(([P.new(1), P.new(2), 3] - [P.new(1), 3]).length())
puts(([P.new(1), P.new(2)] & [P.new(2)]).length())
puts([1, 2, 3, 4].combination(2).to_s())
puts([1, 2, 3, 4].combination(3).to_s())
puts([1, 2, 3].combination(0).to_s())
puts([1, 2, 3].combination(3).to_s())
puts([1, 2, 3].combination(4).to_s())
puts([1, 2, 3, 4, 5].combination(4).length())
puts([].combination(1).to_s())

big = []
60000.times() do |i|
  big.push(i % 20011)
end
puts((big - big.take(30000)).length())
puts((big & big.drop(30000)).length())
puts((0...400).to_a().combination(2).length())
