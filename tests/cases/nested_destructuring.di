class NestedTargets
  def self.load(value)
    [@@head, [@@left, @@right]] = value
  end

  def initialize(value)
    [@name, [@x, @y]] = value
  end

  def values()
    [@name, @x, @y]
  end

  def self.values()
    [@@head, @@left, @@right]
  end
end

[first, [second, third]] = [1, [2, 3]]
fourth, [fifth, sixth] = [4, [5, 6]]
NestedTargets.load([7, [8, 9]])
object = NestedTargets.new(["point", [10, 11]])

stable = "unchanged"
begin
  [stable, [bad_left, bad_right]] = ["changed", [12]]
rescue error: ArgumentError
end

[[first, second, third], [fourth, fifth, sixth],
 NestedTargets.values(), object.values(), stable]
