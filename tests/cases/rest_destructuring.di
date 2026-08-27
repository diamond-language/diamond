class RestTargets
  def self.load(value)
    [@@head, *@@tail] = value
  end

  def initialize(value)
    [@head, [@second, *@tail]] = value
  end

  def values()
    [@head, @second, @tail]
  end

  def self.values()
    [@@head, @@tail]
  end
end

[head, *tail] = [1, 2, 3]
first, [second, *nested_tail] = [4, [5, 6, 7]]
[only, *empty] = [8]
RestTargets.load([9, 10, 11])
object = RestTargets.new([12, [13, 14, 15]])

[[head, tail], [first, second, nested_tail], [only, empty],
 RestTargets.values(), object.values()]
